// main.cpp : fly-by-wire. Sits in the tray, watches the mouse through a low
// level hook, and can drop two kinds of hardware fault before applications see
// them: a click that re-fires by itself, and a wheel tick in the wrong direction.
//
//   fly-by-wire.exe [--port 8479] [--data <folder>] [--web <folder>] [--open]
//
//   --data   where the settings and recordings go (default %LOCALAPPDATA%\fly-by-wire)
//   --web    serve index.html from this folder instead of the copy built into
//            the exe, for working on the page
//   --open   open the tuner page in the browser once started
//   --count-injected   testing only: also judge events that other programs
//            inject, so the scripted mouse (inject.exe) can stand in for a real one
//
// Talking to the copy that is already running (one runs per session):
//   fly-by-wire.exe --off    switch the guard off (works from Win+R when the mouse is the problem)
//   fly-by-wire.exe --on     switch it back on
//   fly-by-wire.exe --quit   stop it cleanly
// Ctrl+Alt+Shift+F11 also switches the guard off, from anywhere.
//
// The rules are in filter.h, the hook in engine.h, what gets recorded in store.h.
#include "http.h"
#include "engine.h"
#include "store.h"

#include <shellapi.h>
#include <cfgmgr32.h>

#include <cmath>
#include <map>
#include <mutex>
#include <string>

using namespace fbw;

namespace {

constexpr UINT kTrayMsg = WM_APP + 10;
constexpr UINT kCommandMsg = WM_APP + 11;      // from a second copy started with --off, --on or --quit
enum Command : int { kCmdOff = 0, kCmdOn = 1, kCmdQuit = 2, kCmdShow = 3 };
constexpr UINT kTimerId = 1;
constexpr int  kHotkeyId = 1;
enum MenuId : UINT { kMenuGuard = 1, kMenuTuner = 2, kMenuQuit = 3, kMenuAutostart = 4 };

// Changes with every build. The page carries the one it was served with and
// reloads itself when the program answers with another.
const char kBuild[] = __DATE__ " " __TIME__;

struct App {
    Engine     engine;
    Store      store;
    HttpServer http;

    std::mutex cfgMu;
    std::mutex commitMu;      // one settings change at a time, from reading the old ones to saving the new
    AppConfig  cfg;

    std::wstring dataDir, webDir;
    int   port = 8479;
    bool  httpUp = false;
    bool  hotkeyOk = false;
    bool  portGiven = false, dataGiven = false;   // on the command line: Start with Windows passes them on
    bool  shutDown = false;
    HWND  hwnd = nullptr;
    HANDLE storeThread = nullptr;
    std::atomic<bool> quit{false};
    std::string page;

    UINT  taskbarCreated = 0;
    bool  trayShown = false, trayState = false;

    // watchdog
    POINT    lastCursor{};
    uint64_t lastCalls = 0;
    int      silentChecks = 0;
    double   lastRehook = -1e9;
    double   rehookWaitMs = 10000;    // doubles while putting the hook back does not help
};

App g;

// ---- small helpers ----------------------------------------------------------

void logLine(const char* text) {
    const std::wstring path = g.dataDir + L"\\fly-by-wire.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    char stamp[32];
    const time_t now = time(nullptr);
    tm local{};
    localtime_s(&local, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    fprintf(f, "%s %s\n", stamp, text);
    fclose(f);
}

std::string readFile(const std::wstring& path) {
    std::string out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// Reads {"key":value,...} one level deep. Values come back as text. Enough for
// the settings file and for what the page posts; nothing else is accepted.
std::map<std::string, std::string> parseFlat(const std::string& s) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (true) {
        const size_t k1 = s.find('"', i);
        if (k1 == std::string::npos) break;
        const size_t k2 = s.find('"', k1 + 1);
        if (k2 == std::string::npos) break;
        const size_t colon = s.find(':', k2 + 1);
        if (colon == std::string::npos) break;
        size_t v = colon + 1;
        while (v < s.size() && (s[v] == ' ' || s[v] == '\t' || s[v] == '\r' || s[v] == '\n')) ++v;
        std::string value;
        if (v < s.size() && s[v] == '"') {
            const size_t e = s.find('"', v + 1);
            if (e == std::string::npos) break;
            value = s.substr(v + 1, e - v - 1);
            i = e + 1;
        } else {
            size_t e = v;
            while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != '\r' && s[e] != '\n' && s[e] != ' ') ++e;
            value = s.substr(v, e - v);
            i = e;
        }
        out[s.substr(k1 + 1, k2 - k1 - 1)] = value;
    }
    return out;
}

double clampTo(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

// Applies the keys that are present, each held to a range where the rule
// still makes sense. Unknown keys are ignored.
void applyFlat(const std::map<std::string, std::string>& m, AppConfig* c) {
    auto flag = [&](const char* key, bool* out) {
        const auto it = m.find(key);
        if (it != m.end()) *out = it->second == "true" || it->second == "1";
    };
    auto number = [&](const char* key, double lo, double hi, double* out) {
        const auto it = m.find(key);
        if (it == m.end()) return;
        char* end = nullptr;
        const double v = strtod(it->second.c_str(), &end);
        if (end != it->second.c_str() && v == v) *out = std::round(clampTo(v, lo, hi));   // whole ms: what runs is what is shown
    };
    // Upper limits. A re-press window above 80 ms eats deliberate double
    // clicks: every click after the first, until the hand pauses. A hold
    // above 50 ms is a release anyone can feel. 0 switches a rule off for
    // that button.
    flag("enabled", &c->enabled);
    flag("clickGuard", &c->rules.clickGuard);
    number("bounceLeft", 0, 80, &c->rules.bounceMs[Left]);
    number("bounceRight", 0, 80, &c->rules.bounceMs[Right]);
    number("bounceMiddle", 0, 80, &c->rules.bounceMs[Middle]);
    double side = c->rules.bounceMs[X1];
    number("bounceSide", 0, 80, &side);
    c->rules.bounceMs[X1] = c->rules.bounceMs[X2] = side;
    number("holdLeft", 0, 50, &c->rules.holdMs[Left]);
    number("holdRight", 0, 50, &c->rules.holdMs[Right]);
    number("holdMiddle", 0, 50, &c->rules.holdMs[Middle]);
    side = c->rules.holdMs[X1];
    number("holdSide", 0, 50, &side);
    c->rules.holdMs[X1] = c->rules.holdMs[X2] = side;
    number("blipMs", 0, 30, &c->rules.blipMs);
    flag("wheelGuard", &c->rules.wheelGuard);
    flag("wheelResend", &c->rules.wheelResend);
    flag("wheelFlip", &c->rules.wheelFlip);
    number("reverseMs", 0, 600, &c->rules.reverseMs);
    number("confirmMs", 20, 500, &c->rules.confirmMs);
    number("turnMs", 0, 5000, &c->rules.turnMs);
    number("firstMs", 0, 150, &c->rules.firstMs);
    double keep = c->keepDays;
    number("keepDays", 0, 3650, &keep);
    c->keepDays = static_cast<int>(keep);
    // Which mouse: "1038:170E" (vendor and product id in hex), or "" for every mouse.
    const auto mouse = m.find("mouse");
    if (mouse != m.end()) {
        unsigned vid = 0, pid = 0;
        if (mouse->second.empty()) {
            c->vid = 0;
            c->pid = 0;
        } else if (mouse->second.size() == 9 && sscanf_s(mouse->second.c_str(), "%4x:%4x", &vid, &pid) == 2) {
            c->vid = static_cast<uint16_t>(vid);
            c->pid = static_cast<uint16_t>(pid);
        }
    }
}

std::string configJson(const AppConfig& c) {
    char mouse[16] = "";
    if (c.vid || c.pid) snprintf(mouse, sizeof(mouse), "%04X:%04X", c.vid, c.pid);
    char buf[1000];
    snprintf(buf, sizeof(buf),
             "{\n  \"enabled\": %s,\n  \"mouse\": \"%s\",\n  \"clickGuard\": %s,\n  \"bounceLeft\": %.0f,\n  \"bounceRight\": %.0f,\n"
             "  \"bounceMiddle\": %.0f,\n  \"bounceSide\": %.0f,\n  \"holdLeft\": %.0f,\n  \"holdRight\": %.0f,\n"
             "  \"holdMiddle\": %.0f,\n  \"holdSide\": %.0f,\n  \"blipMs\": %.0f,\n"
             "  \"wheelGuard\": %s,\n  \"wheelResend\": %s,\n  \"wheelFlip\": %s,\n  \"reverseMs\": %.0f,\n  \"confirmMs\": %.0f,\n"
             "  \"turnMs\": %.0f,\n  \"firstMs\": %.0f,\n  \"keepDays\": %d\n}\n",
             c.enabled ? "true" : "false", mouse, c.rules.clickGuard ? "true" : "false", c.rules.bounceMs[Left],
             c.rules.bounceMs[Right], c.rules.bounceMs[Middle], c.rules.bounceMs[X1], c.rules.holdMs[Left],
             c.rules.holdMs[Right], c.rules.holdMs[Middle], c.rules.holdMs[X1], c.rules.blipMs,
             c.rules.wheelGuard ? "true" : "false", c.rules.wheelResend ? "true" : "false", c.rules.wheelFlip ? "true" : "false",
             c.rules.reverseMs, c.rules.confirmMs, c.rules.turnMs, c.rules.firstMs, c.keepDays);
    return buf;
}

void saveConfig(const AppConfig& c) {
    const std::wstring path = g.dataDir + L"\\config.json";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    const std::string j = configJson(c);
    fwrite(j.data(), 1, j.size(), f);
    fclose(f);
}

AppConfig currentConfig() {
    std::lock_guard<std::mutex> lock(g.cfgMu);
    return g.cfg;
}

// The one place settings change: memory, the hook, and the file together.
void commitConfig(const AppConfig& c) {
    {
        std::lock_guard<std::mutex> lock(g.cfgMu);
        g.cfg = c;
    }
    g.engine.setConfig(c.rules, c.enabled, c.vid, c.pid);
    g.store.noteConfig(c, g.engine.nowMs());
    saveConfig(c);
}

// ---- tray -------------------------------------------------------------------

// Round icon drawn in code: a filled disc while the guard is on, a ring while off.
HICON makeIcon(bool on) {
    const int n = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = n;
    bi.bV5Height = -n;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return LoadIconW(nullptr, IDI_APPLICATION);
    auto* px = static_cast<uint32_t*>(bits);
    const double r = on ? 0x58 : 0x8B, gr = on ? 0xA6 : 0x94, b = on ? 0xFF : 0x9E;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const double dx = x - 15.5, dy = y - 15.5;
            const double d = std::sqrt(dx * dx + dy * dy);
            double a = clampTo(13.5 - d, 0, 1);
            if (!on) a *= clampTo(d - 8.5, 0, 1);
            const uint32_t A = static_cast<uint32_t>(a * 255.0);
            const uint32_t R = static_cast<uint32_t>(r * a), G = static_cast<uint32_t>(gr * a), B = static_cast<uint32_t>(b * a);
            px[y * n + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

void trayShow(bool on) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = makeIcon(on);
    wcscpy_s(nid.szTip, !on ? L"fly-by-wire: guard off"
                        : g.hotkeyOk ? L"fly-by-wire: guard on\nCtrl+Alt+Shift+F11 switches it off"
                                     : L"fly-by-wire: guard on");
    Shell_NotifyIconW(g.trayShown ? NIM_MODIFY : NIM_ADD, &nid);
    if (nid.hIcon) DestroyIcon(nid.hIcon);
    g.trayShown = true;
    g.trayState = on;
}

void trayRemove() {
    if (!g.trayShown) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g.trayShown = false;
}

void openTuner() {
    wchar_t url[64];
    swprintf_s(url, L"http://127.0.0.1:%d/", g.port);
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// Changes the settings as one step. Two changes at once (the page and the
// hotkey, say) must not interleave, or the hook could end up running settings
// that are neither saved nor shown.
template <typename Change>
void updateConfig(Change change) {
    std::lock_guard<std::mutex> lock(g.commitMu);
    AppConfig c = currentConfig();
    change(&c);
    keepSane(&c.rules);     // as at start-up: the page changes one field at a time
    commitConfig(c);
}

void setGuard(bool on) {
    updateConfig([on](AppConfig* c) { c->enabled = on; });
    trayShow(on);
}

// ---- start with Windows -------------------------------------------------------
//
// A value under the user's Run key, set on the first run and removed or set
// again from the tray menu. Nothing is installed and nothing else is written.

const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"fly-by-wire";

std::wstring runCommand() {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return std::wstring();
    std::wstring command = L"\"" + std::wstring(path, n) + L"\"";
    if (g.portGiven) command += L" --port " + std::to_wstring(g.port);
    if (g.dataGiven) command += L" --data \"" + g.dataDir + L"\"";
    return command;
}

// True when Windows will start this very copy at login.
bool startsWithWindows() {
    wchar_t value[MAX_PATH * 2 + 8] = {};
    DWORD size = sizeof(value) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
    const std::wstring want = runCommand();
    return !want.empty() && _wcsicmp(value, want.c_str()) == 0;
}

void setStartWithWindows(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const std::wstring command = runCommand();
    if (on && !command.empty()) {
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else if (!on) {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

void showMenu() {
    HMENU menu = CreatePopupMenu();
    const bool on = currentConfig().enabled;
    AppendMenuW(menu, MF_STRING | (on ? MF_CHECKED : 0), kMenuGuard, L"Guard");
    const bool autostart = startsWithWindows();
    AppendMenuW(menu, MF_STRING | (autostart ? MF_CHECKED : 0), kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_STRING | (g.httpUp ? 0 : MF_GRAYED), kMenuTuner, L"Tuner");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g.hwnd);   // needed so the menu closes when clicking elsewhere
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd == kMenuGuard) setGuard(!on);
    else if (cmd == kMenuAutostart) setStartWithWindows(!autostart);
    else if (cmd == kMenuTuner) openTuner();
    else if (cmd == kMenuQuit) DestroyWindow(g.hwnd);
}

// Windows removes a low level hook without telling anyone if its callback was
// ever too slow. A moving cursor always produces callbacks, so a cursor that
// moved while the call count stood still means the hook is gone: put it back.
//
// The same picture also shows without the hook being gone: while a window that
// runs as administrator has the foreground this program is not called at all,
// and a program can move the cursor by itself. Putting the hook back does no
// harm then, but it does no good either, so each try that is followed by more
// silence doubles the wait before the next one, and only the first is logged.
void watchdog() {
    POINT p;
    const bool known = GetCursorPos(&p) != FALSE;       // fails on the lock screen
    const uint64_t calls = g.engine.hookCalls.load();
    if (calls != g.lastCalls) g.rehookWaitMs = 10000;   // the hook is being called: it is alive
    const bool moved = known && (p.x != g.lastCursor.x || p.y != g.lastCursor.y);
    if (moved && calls == g.lastCalls) {
        const double now = g.engine.nowMs();
        if (++g.silentChecks >= 2 && now - g.lastRehook > g.rehookWaitMs) {
            g.engine.rehook();
            g.lastRehook = now;
            g.silentChecks = 0;
            if (g.rehookWaitMs <= 10000) logLine("hook was silent while the cursor moved: reinstalled");
            if (g.rehookWaitMs < 300000) g.rehookWaitMs *= 2;
        }
    } else {
        g.silentChecks = 0;
    }
    if (known) g.lastCursor = p;
    g.lastCalls = calls;

    // keep the tray icon in step with changes made on the page
    const bool on = currentConfig().enabled;
    if (on != g.trayState) trayShow(on);
}

void shutdownAll();
void noteNewDevices();

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kTrayMsg:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
            else if (LOWORD(lp) == WM_LBUTTONUP && g.httpUp) openTuner();
            return 0;
        case WM_HOTKEY:
            // Off only. A key that could also switch the guard on would leave
            // its state unknown to someone who cannot see the tray. And off
            // whatever this program believes the state to be: the key is for
            // when something is wrong.
            if (wp == kHotkeyId) setGuard(false);
            return 0;
        case kCommandMsg: {
            const int command = static_cast<int>(wp);
            if (command == kCmdQuit) DestroyWindow(hwnd);
            else if (command == kCmdOff) setGuard(false);
            else if (command == kCmdOn) setGuard(true);
            else if (command == kCmdShow && g.httpUp) openTuner();
            return 0;
        }
        case WM_ENDSESSION:
            // Signing out or shutting down: Windows ends the process after this
            // returns, so finish the test and close the recording now.
            if (wp) shutdownAll();
            return 0;
        case WM_TIMER:
            if (wp == kTimerId) watchdog();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            if (msg == g.taskbarCreated && g.taskbarCreated) {
                g.trayShown = false;      // explorer restarted and forgot the icon
                trayShow(currentConfig().enabled);
                return 0;
            }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- http -------------------------------------------------------------------

// Puts this build's stamp where the page expects it.
std::string stampBuild(std::string html) {
    static const char kSlot[] = "__FBW_BUILD__";
    const size_t at = html.find(kSlot);
    if (at != std::string::npos) html.replace(at, sizeof(kSlot) - 1, kBuild);
    return html;
}

// Everything that has to happen before the process ends, once.
void shutdownAll() {
    if (g.shutDown) return;
    g.shutDown = true;
    KillTimer(g.hwnd, kTimerId);
    UnregisterHotKey(g.hwnd, kHotkeyId);
    trayRemove();
    g.http.stop();
    g.engine.stop();      // sends any release that was being held back, then unhooks
    g.quit.store(true);
    if (g.storeThread) {
        WaitForSingleObject(g.storeThread, 2000);
        CloseHandle(g.storeThread);
        g.storeThread = nullptr;
    }
    noteNewDevices();
    Rec r;
    while (g.engine.popRecord(&r)) g.store.add(r);
    g.store.close();      // finishes a test that is still running
    logLine("stopped");
}

uint64_t querySince(const std::string& query) {
    const size_t p = query.find("since=");
    return p == std::string::npos ? 0 : strtoull(query.c_str() + p + 6, nullptr, 10);
}

HttpResponse handle(const HttpRequest& req, void*) {
    HttpResponse res;
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        res.type = "text/html; charset=utf-8";
        res.body = g.webDir.empty() ? g.page : stampBuild(readFile(g.webDir + L"\\index.html"));
        return res;
    }
    if (req.method == "GET" && req.path == "/api/state") {
        AppInfo info;
        info.port = g.port;
        info.hotkey = g.hotkeyOk;
        info.build = kBuild;
        res.body = g.store.stateJson(querySince(req.query), currentConfig(), g.engine, g.engine.nowMs(), info);
        return res;
    }
    if (req.method == "POST" && req.path == "/api/config") {
        const auto posted = parseFlat(req.body);
        updateConfig([&posted](AppConfig* c) { applyFlat(posted, c); });
        res.body = "{\"ok\":true}";
        return res;
    }
    if (req.method == "POST" && req.path == "/api/test") {
        const auto m = parseFlat(req.body);
        const auto action = m.find("action");
        bool ok = false;
        if (action != m.end() && action->second == "stop") {
            g.store.stopTest(g.engine.nowMs());
            ok = true;
        } else if (action != m.end() && action->second == "start") {
            const auto name = m.find("test");
            const auto target = m.find("target");
            int id = kNoTest;
            for (int i = 1; i < kTestCount; ++i) if (name != m.end() && name->second == testName(i)) id = i;
            const AppConfig c = currentConfig();
            bool anyHold = false;
            for (double h : c.rules.holdMs) anyHold = anyHold || h > 0;
            const bool guarded = c.enabled && (id == kTestClicks ? (c.rules.clickGuard || anyHold) : c.rules.wheelGuard);
            const int want = target != m.end() ? atoi(target->second.c_str()) : 0;
            ok = g.store.startTest(id, want, guarded, g.engine.nowMs());
        }
        res.status = ok ? 200 : 400;
        res.body = ok ? "{\"ok\":true}" : "{\"ok\":false}";
        return res;
    }
    if (req.method == "POST" && req.path == "/api/reset") {
        g.store.resetStats();
        res.body = "{\"ok\":true}";
        return res;
    }
    res.status = 404;
    res.type = "text/plain";
    res.body = "not found";
    return res;
}

// What a mouse calls itself, for example "SteelSeries Rival 500 Gaming Mouse".
// The mouse part of a device has no name of its own ("HID-compliant mouse");
// the name the device reported when it was plugged in sits one or two levels
// up in the device tree. Empty when nothing there has one.
std::string productName(const Device& d) {
    // DEVPKEY_Device_BusReportedDeviceDesc
    static const DEVPROPKEY kBusName = {{0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4};
    std::string out;
    // "\\?\HID#VID_1038&PID_170E&MI_01&Col01#7&1a2b3c4d&0&0000#{...}" names the
    // device "HID\VID_1038&PID_170E&MI_01&Col01\7&1a2b3c4d&0&0000".
    std::wstring id = d.path;
    if (id.rfind(L"\\\\?\\", 0) == 0) id.erase(0, 4);
    const size_t brace = id.rfind(L"#{");
    if (brace == std::wstring::npos) return out;
    id.resize(brace);
    for (wchar_t& c : id) if (c == L'#') c = L'\\';
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, &id[0], CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return out;
    for (int up = 0; up < 3 && out.empty(); ++up) {
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
        wchar_t name[200] = {};
        ULONG size = sizeof(name) - sizeof(wchar_t);
        DEVPROPTYPE type = 0;
        if (CM_Get_DevNode_PropertyW(node, &kBusName, &type, reinterpret_cast<PBYTE>(name), &size, 0) == CR_SUCCESS &&
            type == DEVPROP_TYPE_STRING) {
            for (const wchar_t* c = name; *c; ++c) out += (*c >= 32 && *c < 127) ? static_cast<char>(*c) : '?';
        }
    }
    return out;
}

// Tells the store about mice the hook has met since the last call. Runs on
// the store thread, before the records that mention them are taken in.
void noteNewDevices() {
    static int noted = 0;
    while (noted < g.engine.deviceCount()) {
        Device d;
        if (g.engine.device(noted + 1, &d)) g.store.noteDevice(noted + 1, d, productName(d));
        ++noted;
    }
}

DWORD WINAPI storeLoop(void*) {
    while (!g.quit.load()) {
        WaitForSingleObject(g.engine.recordEvent(), 100);
        noteNewDevices();
        Rec r;
        while (g.engine.popRecord(&r)) g.store.add(r);
        g.store.poll(g.engine.nowMs());
    }
    return 0;
}

std::string loadPageResource() {
    HRSRC found = FindResourceW(nullptr, L"PAGE", RT_RCDATA);
    if (!found) return "<!doctype html><title>fly-by-wire</title><p>page resource missing";
    HGLOBAL loaded = LoadResource(nullptr, found);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(nullptr, found);
    return data ? std::string(static_cast<const char*>(data), size) : std::string();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    bool open = false;
    int command = -1;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--open") open = true;
        else if (a == L"--off") command = kCmdOff;
        else if (a == L"--on") command = kCmdOn;
        else if (a == L"--quit") command = kCmdQuit;
        else if (a == L"--count-injected") g.engine.treatInjectedAsReal = true;
        else if (a == L"--port" && i + 1 < argc) { g.port = _wtoi(argv[++i]); g.portGiven = true; }
        else if (a == L"--data" && i + 1 < argc) { g.dataDir = argv[++i]; g.dataGiven = true; }
        else if (a == L"--web" && i + 1 < argc) g.webDir = argv[++i];
    }
    if (argv) LocalFree(argv);
    if (g.port < 1024 || g.port > 65535) g.port = 8479;

    // One copy per user session: a second hook would judge every event twice.
    // Access denied means a copy with more rights already holds the name.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\fly-by-wire");
    const DWORD mutexError = GetLastError();
    bool alone = single && mutexError != ERROR_ALREADY_EXISTS;
    if (!alone) {
        const HWND running = FindWindowW(L"fly-by-wire", L"fly-by-wire");
        if (command >= 0) {
            // A copy that runs with more rights does not take messages from
            // this one. Say so with the exit code instead of pretending.
            if (!running) return 2;
            return PostMessageW(running, kCommandMsg, static_cast<WPARAM>(command), 0) ? 0 : 2;
        }
        if (running) {
            // Started again while running: show the page of the copy that runs
            // (it knows its own port), so starting it is never a silent no-op.
            PostMessageW(running, kCommandMsg, static_cast<WPARAM>(kCmdShow), 0);
            return 0;
        }
        // The name is taken but no copy has a window: one is just leaving (or
        // just starting). Wait for it to go rather than exit and leave nothing
        // running.
        const DWORD waited = single ? WaitForSingleObject(single, 4000) : WAIT_FAILED;
        if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) return 0;
        alone = true;
    }
    if (command >= 0) return 0;      // a command for a copy that is not running

    if (g.dataDir.empty()) {
        wchar_t base[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        g.dataDir = (n > 0 && n < MAX_PATH) ? std::wstring(base) + L"\\fly-by-wire" : L".";
    }
    CreateDirectoryW(g.dataDir.c_str(), nullptr);

    // Settings: the guard on for every mouse, then whatever was saved.
    const std::string saved = readFile(g.dataDir + L"\\config.json");
    if (!saved.empty()) applyFlat(parseFlat(saved), &g.cfg);
    keepSane(&g.cfg.rules);
    const bool firstRun = saved.empty();     // nothing saved yet: show the page, there is no other sign of life but a tray icon
    if (firstRun) {                          // and only this once:
        saveConfig(g.cfg);
        setStartWithWindows(true);           // on by default, so the guard is there after every login; the tray menu takes it off
    }

    g.page = stampBuild(loadPageResource());
    g.store.open(g.dataDir);

    if (!g.engine.start()) {
        logLine("could not install the mouse hook");
        MessageBoxW(nullptr, L"fly-by-wire could not install its mouse hook.", L"fly-by-wire", MB_ICONERROR);
        return 1;
    }
    g.engine.setConfig(g.cfg.rules, g.cfg.enabled, g.cfg.vid, g.cfg.pid);
    g.store.noteConfig(g.cfg, g.engine.nowMs());
    if (!g.engine.rawInput.load()) logLine("raw input could not be registered: mice cannot be told apart, so only \"every mouse\" will filter");
    g.storeThread = CreateThread(nullptr, 0, &storeLoop, nullptr, 0, nullptr);

    // Another program, or this program in another user's session, may have
    // the port: take the next free one.
    for (int p = g.port; p < g.port + 10 && p <= 65535 && !g.httpUp; ++p) {
        g.httpUp = g.http.start(static_cast<uint16_t>(p), &handle, nullptr);
        if (g.httpUp) g.port = p;
    }
    if (!g.httpUp) logLine("could not listen on the tuner port: the guard runs without its page");

    WNDCLASSW wc{};
    wc.lpfnWndProc = &wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"fly-by-wire";
    RegisterClassW(&wc);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"fly-by-wire", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // A copy running as administrator still takes --off, --on, --quit from a normal window.
    ChangeWindowMessageFilterEx(g.hwnd, kCommandMsg, MSGFLT_ALLOW, nullptr);

    // Keyboard way out for when the mouse itself is the problem. Not F12:
    // Windows keeps that key for debuggers. Another program may already own
    // the combination, so the result is kept and shown.
    g.hotkeyOk = RegisterHotKey(g.hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F11) != 0;
    if (!g.hotkeyOk) logLine("Ctrl+Alt+Shift+F11 is taken by another program: no keyboard off switch");
    trayShow(g.cfg.enabled);
    GetCursorPos(&g.lastCursor);
    SetTimer(g.hwnd, kTimerId, 2000, nullptr);
    logLine("started");
    if ((open || firstRun) && g.httpUp) openTuner();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    shutdownAll();
    if (single) CloseHandle(single);
    return 0;
}
