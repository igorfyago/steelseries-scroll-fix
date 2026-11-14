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
// The rules are in filter.h, the hook in engine.h, what gets recorded in store.h.
#include "http.h"
#include "engine.h"
#include "store.h"

#include <shellapi.h>

#include <cmath>
#include <map>
#include <mutex>
#include <string>

using namespace fbw;

namespace {

constexpr UINT kTrayMsg = WM_APP + 10;
constexpr UINT kTimerId = 1;
constexpr int  kHotkeyId = 1;
enum MenuId : UINT { kMenuGuard = 1, kMenuTuner = 2, kMenuQuit = 3 };

struct App {
    Engine     engine;
    Store      store;
    HttpServer http;

    std::mutex cfgMu;
    AppConfig  cfg;

    std::wstring dataDir, webDir;
    int   port = 8479;
    bool  httpUp = false;
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
        if (end != it->second.c_str() && v == v) *out = clampTo(v, lo, hi);
    };
    flag("enabled", &c->enabled);
    flag("clickGuard", &c->rules.clickGuard);
    number("bounceLeft", 0, 150, &c->rules.bounceMs[Left]);
    number("bounceRight", 0, 150, &c->rules.bounceMs[Right]);
    number("bounceMiddle", 0, 150, &c->rules.bounceMs[Middle]);
    double side = c->rules.bounceMs[X1];
    number("bounceSide", 0, 150, &side);
    c->rules.bounceMs[X1] = c->rules.bounceMs[X2] = side;
    flag("holdGuard", &c->rules.holdGuard);
    number("holdMs", 1, 60, &c->rules.holdMs);
    flag("wheelGuard", &c->rules.wheelGuard);
    number("reverseMs", 0, 600, &c->rules.reverseMs);
    number("confirmMs", 20, 1000, &c->rules.confirmMs);
}

std::string configJson(const AppConfig& c) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\n  \"enabled\": %s,\n  \"clickGuard\": %s,\n  \"bounceLeft\": %.0f,\n  \"bounceRight\": %.0f,\n"
             "  \"bounceMiddle\": %.0f,\n  \"bounceSide\": %.0f,\n  \"holdGuard\": %s,\n  \"holdMs\": %.0f,\n"
             "  \"wheelGuard\": %s,\n  \"reverseMs\": %.0f,\n  \"confirmMs\": %.0f\n}\n",
             c.enabled ? "true" : "false", c.rules.clickGuard ? "true" : "false", c.rules.bounceMs[Left],
             c.rules.bounceMs[Right], c.rules.bounceMs[Middle], c.rules.bounceMs[X1],
             c.rules.holdGuard ? "true" : "false", c.rules.holdMs, c.rules.wheelGuard ? "true" : "false",
             c.rules.reverseMs, c.rules.confirmMs);
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
    g.engine.setConfig(c.rules, c.enabled);
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
    wcscpy_s(nid.szTip, on ? L"fly-by-wire: guard on" : L"fly-by-wire: guard off");
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

void setGuard(bool on) {
    AppConfig c = currentConfig();
    c.enabled = on;
    commitConfig(c);
    trayShow(on);
}

void showMenu() {
    HMENU menu = CreatePopupMenu();
    const bool on = currentConfig().enabled;
    AppendMenuW(menu, MF_STRING | (on ? MF_CHECKED : 0), kMenuGuard, L"Guard");
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
    else if (cmd == kMenuTuner) openTuner();
    else if (cmd == kMenuQuit) DestroyWindow(g.hwnd);
}

// Windows removes a low level hook without telling anyone if its callback was
// ever too slow. A moving cursor always produces callbacks, so a cursor that
// moved while the call count stood still means the hook is gone: put it back.
void watchdog() {
    POINT p;
    GetCursorPos(&p);
    const uint64_t calls = g.engine.hookCalls.load();
    const bool moved = p.x != g.lastCursor.x || p.y != g.lastCursor.y;
    if (moved && calls == g.lastCalls) {
        const double now = g.engine.nowMs();
        if (++g.silentChecks >= 2 && now - g.lastRehook > 10000) {
            g.engine.rehook();
            g.lastRehook = now;
            g.silentChecks = 0;
            logLine("hook was silent while the cursor moved: reinstalled");
        }
    } else {
        g.silentChecks = 0;
    }
    g.lastCursor = p;
    g.lastCalls = calls;

    // keep the tray icon in step with changes made on the page
    const bool on = currentConfig().enabled;
    if (on != g.trayState) trayShow(on);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kTrayMsg:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
            else if (LOWORD(lp) == WM_LBUTTONUP) openTuner();
            return 0;
        case WM_HOTKEY:
            if (wp == kHotkeyId) setGuard(!currentConfig().enabled);
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

uint64_t querySince(const std::string& query) {
    const size_t p = query.find("since=");
    return p == std::string::npos ? 0 : strtoull(query.c_str() + p + 6, nullptr, 10);
}

HttpResponse handle(const HttpRequest& req, void*) {
    HttpResponse res;
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        res.type = "text/html; charset=utf-8";
        res.body = g.webDir.empty() ? g.page : readFile(g.webDir + L"\\index.html");
        return res;
    }
    if (req.method == "GET" && req.path == "/api/state") {
        res.body = g.store.stateJson(querySince(req.query), currentConfig(), g.engine, g.engine.nowMs(), g.port);
        return res;
    }
    if (req.method == "POST" && req.path == "/api/config") {
        AppConfig c = currentConfig();
        applyFlat(parseFlat(req.body), &c);
        commitConfig(c);
        res.body = "{\"ok\":true}";
        return res;
    }
    if (req.method == "POST" && req.path == "/api/test") {
        const auto m = parseFlat(req.body);
        const auto action = m.find("action");
        bool ok = false;
        if (action != m.end() && action->second == "stop") {
            g.store.stopTest();
            ok = true;
        } else if (action != m.end() && action->second == "start") {
            const auto name = m.find("test");
            const auto target = m.find("target");
            int id = kNoTest;
            for (int i = 1; i < kTestCount; ++i) if (name != m.end() && name->second == testName(i)) id = i;
            const AppConfig c = currentConfig();
            const bool guarded = c.enabled && (id == kTestClicks ? (c.rules.clickGuard || c.rules.holdGuard) : c.rules.wheelGuard);
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

DWORD WINAPI storeLoop(void*) {
    while (!g.quit.load()) {
        WaitForSingleObject(g.engine.recordEvent(), 100);
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
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--open") open = true;
        else if (a == L"--count-injected") g.engine.treatInjectedAsReal = true;
        else if (a == L"--port" && i + 1 < argc) g.port = _wtoi(argv[++i]);
        else if (a == L"--data" && i + 1 < argc) g.dataDir = argv[++i];
        else if (a == L"--web" && i + 1 < argc) g.webDir = argv[++i];
    }
    if (argv) LocalFree(argv);
    if (g.port < 1024 || g.port > 65535) g.port = 8479;

    // One copy per user session: a second hook would judge every event twice.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\fly-by-wire");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (open) openTuner();
        return 0;
    }

    if (g.dataDir.empty()) {
        wchar_t base[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        g.dataDir = (n > 0 && n < MAX_PATH) ? std::wstring(base) + L"\\fly-by-wire" : L".";
    }
    CreateDirectoryW(g.dataDir.c_str(), nullptr);

    // Settings: start from "record only", then whatever was saved.
    g.cfg.enabled = false;
    const std::string saved = readFile(g.dataDir + L"\\config.json");
    if (!saved.empty()) applyFlat(parseFlat(saved), &g.cfg);

    g.page = loadPageResource();
    g.store.open(g.dataDir);

    if (!g.engine.start()) {
        logLine("could not install the mouse hook");
        MessageBoxW(nullptr, L"fly-by-wire could not install its mouse hook.", L"fly-by-wire", MB_ICONERROR);
        return 1;
    }
    g.engine.setConfig(g.cfg.rules, g.cfg.enabled);
    g.storeThread = CreateThread(nullptr, 0, &storeLoop, nullptr, 0, nullptr);

    g.httpUp = g.http.start(static_cast<uint16_t>(g.port), &handle, nullptr);
    if (!g.httpUp) logLine("could not listen on the tuner port: the guard runs without its page");

    WNDCLASSW wc{};
    wc.lpfnWndProc = &wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"fly-by-wire";
    RegisterClassW(&wc);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"fly-by-wire", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    trayShow(g.cfg.enabled);

    // Keyboard way out for when the mouse itself is the problem.
    RegisterHotKey(g.hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
    GetCursorPos(&g.lastCursor);
    SetTimer(g.hwnd, kTimerId, 2000, nullptr);
    logLine("started");
    if (open && g.httpUp) openTuner();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(g.hwnd, kTimerId);
    UnregisterHotKey(g.hwnd, kHotkeyId);
    trayRemove();
    g.http.stop();
    g.engine.stop();      // sends any release that was being held back, then unhooks
    g.quit.store(true);
    if (g.storeThread) {
        WaitForSingleObject(g.storeThread, 2000);
        CloseHandle(g.storeThread);
    }
    Rec r;
    while (g.engine.popRecord(&r)) g.store.add(r);
    g.store.close();
    logLine("stopped");
    if (single) CloseHandle(single);
    return 0;
}
