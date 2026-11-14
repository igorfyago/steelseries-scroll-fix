// selftest.cpp : checks the whole path on the real desktop, not just the rules.
// It opens a small window under the cursor, sends scripted clicks and wheel
// ticks into the system the way a mouse would, and compares what that window
// receives with what the rules say it should receive.
//
// It takes over the mouse for about twenty seconds: the cursor is held inside the
// test window so no scripted click can land on anything else.
//
//   selftest.exe          exit code 0 when every scenario passes
#include "engine.h"

#include <mmsystem.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace fbw;

namespace {

struct Got { double t; char what; };

Engine           g_engine;
HWND             g_hwnd = nullptr;
std::mutex       g_mu;
std::vector<Got> g_got;
int              g_failed = 0;

void note(char what) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_got.push_back({g_engine.nowMs(), what});
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN: note('D'); return 0;
        case WM_LBUTTONUP:   note('U'); return 0;
        case WM_MOUSEWHEEL:  note(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? '+' : '-'); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(0x16, 0x1b, 0x22));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0xe6, 0xed, 0xf3));
            DrawTextW(dc, L"fly-by-wire self test\nhands off the mouse", -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void inject(DWORD flags, int data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = static_cast<DWORD>(data);
    SendInput(1, &in, sizeof(in));
}

void waitUntil(double t) {
    for (;;) {
        const double left = t - g_engine.nowMs();
        if (left <= 0) return;
        if (left > 2.5) Sleep(1);
    }
}

struct Step { double at; char what; };   // D U + - as in Got

struct Scenario {
    const char*       name;
    bool              enabled;
    Config            rules;
    std::vector<Step> steps;
    const char*       expect;
    double            lastAtLeast;   // earliest time for the last received event, 0 to skip
    double            lateTimerMs;   // make the hold timer fire this much late
};

void run(const Scenario& s) {
    g_engine.testTimerLateMs.store(s.lateTimerMs);
    g_engine.setConfig(s.rules, s.enabled);
    Sleep(600);                       // let the setting land and earlier timing go stale
    { std::lock_guard<std::mutex> lock(g_mu); g_got.clear(); }
    Rec r;
    while (g_engine.popRecord(&r)) {}

    const double t0 = g_engine.nowMs();
    for (const Step& st : s.steps) {
        waitUntil(t0 + st.at);
        switch (st.what) {
            case 'D': inject(MOUSEEVENTF_LEFTDOWN); break;
            case 'U': inject(MOUSEEVENTF_LEFTUP); break;
            case '+': inject(MOUSEEVENTF_WHEEL, 120); break;
            default:  inject(MOUSEEVENTF_WHEEL, -120); break;
        }
    }
    Sleep(450);

    std::string got;
    double lastT = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (const Got& g : g_got) { got += g.what; lastT = g.t - t0; }
    }
    int blocked = 0;
    while (g_engine.popRecord(&r)) if (r.blocked) ++blocked;

    bool ok = got == s.expect;
    if (ok && s.lastAtLeast > 0 && (lastT < s.lastAtLeast - 2 || lastT > s.lastAtLeast + 40)) ok = false;
    if (!ok) ++g_failed;
    printf("%s  %-52s sent %zu, window got \"%s\" (want \"%s\"), blocked %d", ok ? "PASS" : "FAIL", s.name,
           s.steps.size(), got.c_str(), s.expect, blocked);
    if (s.lastAtLeast > 0) printf(", last at %.1f ms (want about %.0f)", lastT, s.lastAtLeast);
    printf("\n");
}

DWORD WINAPI driver(void*) {
    Config off;
    off.clickGuard = false;
    off.wheelGuard = false;
    off.firstMs = 0;
    off.wheelFlip = false;
    Config c1 = off;
    c1.clickGuard = true;
    for (double& b : c1.bounceMs) b = 40;
    Config c2 = off;
    c2.holdMs[Left] = 25;
    // the wheel rule with its extras off: a suspect tick is dropped, the first tick is not held
    Config w1 = off;
    w1.wheelGuard = true; w1.reverseMs = 150; w1.confirmMs = 200;
    // the wheel rule as fitted to the Rival 500
    Config fit = off;
    fit.wheelGuard = true; fit.reverseMs = 70; fit.confirmMs = 50; fit.firstMs = 40; fit.wheelFlip = true;

    const Scenario all[] = {
        {"guard off: everything arrives", false, c1,
         {{0, 'D'}, {40, 'U'}, {45, 'D'}, {50, 'U'}, {150, '+'}, {180, '-'}, {210, '+'}}, "DUDU+-+", 0, 0},
        {"C1: re-press 5 ms after release is dropped", true, c1,
         {{0, 'D'}, {50, 'U'}, {55, 'D'}, {60, 'U'}, {360, 'D'}, {410, 'U'}}, "DUDU", 0, 0},
        {"C1: a double click by hand (70 ms apart) passes", true, c1,
         {{0, 'D'}, {60, 'U'}, {130, 'D'}, {190, 'U'}}, "DUDU", 0, 0},
        {"C2: brief opening during a hold is bridged", true, c2,
         {{0, 'D'}, {60, 'U'}, {66, 'D'}, {260, 'U'}}, "DU", 285, 0},
        // The hold timer is made to fire 40 ms late, so the second press
        // arrives after the hold ran out but before the timer got to send the
        // release. The release must still reach the window before that press.
        {"C2: a press that beats the late timer still comes after the release", true, c2,
         {{0, 'D'}, {60, 'U'}, {95, 'D'}, {200, 'U'}}, "DUDU", 0, 40},
        {"wheel, dropping: stray reversed tick in mid scroll is dropped", true, w1,
         {{0, '+'}, {30, '+'}, {60, '-'}, {90, '+'}}, "+++", 0, 0},
        {"wheel, dropping: real reversal delivers both ticks", true, w1,
         {{0, '+'}, {40, '-'}, {80, '-'}}, "+--", 0, 0},
        {"wheel, fitted: a misread tick in mid stroke is turned round", true, fit,
         {{0, '+'}, {10, '+'}, {20, '+'}, {24, '-'}, {32, '+'}}, "+++++", 0, 0},
        {"wheel, fitted: misread first tick, the third tick decides", true, fit,
         {{0, '-'}, {10, '+'}, {20, '+'}}, "+++", 0, 0},
        {"wheel, fitted: misread second tick, the first is not blamed", true, fit,
         {{0, '+'}, {10, '-'}, {20, '+'}}, "+++", 0, 0},
        {"wheel, fitted: a single notch arrives 40 ms late", true, fit,
         {{0, '+'}}, "+", 40, 0},
        {"wheel, fitted: a misread last tick is turned round, 50 ms late", true, fit,
         {{0, '+'}, {10, '+'}, {30, '-'}}, "+++", 80, 0},
        {"wheel, fitted: a real turn after a pause keeps every notch", true, fit,
         {{0, '+'}, {10, '+'}, {150, '-'}, {160, '-'}}, "++--", 0, 0},
        {"guard switched off again: the bounce and the stray tick arrive", false, fit,
         {{0, 'D'}, {50, 'U'}, {55, 'D'}, {60, 'U'}, {150, '+'}, {160, '+'}, {164, '-'}, {172, '+'}}, "DUDU++-+", 0, 0},
    };
    Sleep(300);
    for (const Scenario& s : all) run(s);
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    return 0;
}

}  // namespace

int main() {
    timeBeginPeriod(1);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = &wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"fly-by-wire-selftest";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    POINT p;
    GetCursorPos(&p);
    const int size = 280;
    HWND previous = GetForegroundWindow();
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"fly-by-wire self test",
                             WS_POPUP | WS_VISIBLE, p.x - size / 2, p.y - size / 2, size, size,
                             nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) { printf("could not create the test window\n"); return 2; }
    UpdateWindow(g_hwnd);

    RECT old{};
    GetClipCursor(&old);
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    InflateRect(&rc, -40, -40);
    ClipCursor(&rc);                   // no scripted click can land outside this window

    g_engine.treatInjectedAsReal = true;
    if (!g_engine.start()) {
        ClipCursor(&old);
        printf("could not install the hook\n");
        return 2;
    }

    HANDLE thread = CreateThread(nullptr, 0, &driver, nullptr, 0, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ClipCursor(&old);
    WaitForSingleObject(thread, 2000);
    CloseHandle(thread);
    g_engine.stop();
    if (previous) SetForegroundWindow(previous);
    timeEndPeriod(1);

    printf("%s: %d scenario(s) failed, %u send failures\n", g_failed ? "SELF TEST FAILED" : "SELF TEST PASSED",
           g_failed, g_engine.sendFailures.load());
    return g_failed ? 1 : 0;
}
