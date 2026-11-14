// inject.cpp : a scripted mouse with known faults, for checking that the
// recorder counts correctly before anyone spends real clicks on it.
//
// It opens a small window under the cursor, holds the cursor inside it, and
// sends a pattern into the system the way a mouse would:
//
//   inject.exe clicks <n> <everyK>   n left clicks, every k-th one re-fires 5 ms after release
//   inject.exe up     <n> <everyK>   n wheel ticks up, an extra tick down after every k-th
//   inject.exe down   <n> <everyK>   n wheel ticks down, an extra tick up after every k-th
//
// What it sends is marked as injected by Windows, so fly-by-wire only counts
// it when started with --count-injected.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

HWND g_hwnd = nullptr;
const char* g_mode = "";
int g_n = 0, g_every = 0, g_faults = 0;
LARGE_INTEGER g_freq, g_origin;

double nowMs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart - g_origin.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
}

void waitUntil(double t) {
    for (;;) {
        const double left = t - nowMs();
        if (left <= 0) return;
        if (left > 2.5) Sleep(1);
    }
}

void send(DWORD flags, int data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = static_cast<DWORD>(data);
    SendInput(1, &in, sizeof(in));
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0x16, 0x1b, 0x22));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0xe6, 0xed, 0xf3));
        DrawTextW(dc, L"fly-by-wire scripted mouse\nhands off the mouse", -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI driver(void*) {
    Sleep(500);
    double t = nowMs();
    if (strcmp(g_mode, "clicks") == 0) {
        for (int i = 1; i <= g_n; ++i) {
            waitUntil(t);       send(MOUSEEVENTF_LEFTDOWN);
            waitUntil(t + 40);  send(MOUSEEVENTF_LEFTUP);
            if (g_every > 0 && i % g_every == 0) {
                waitUntil(t + 45);  send(MOUSEEVENTF_LEFTDOWN);
                waitUntil(t + 48);  send(MOUSEEVENTF_LEFTUP);
                ++g_faults;
            }
            t += 300;
        }
    } else {
        const int dir = strcmp(g_mode, "up") == 0 ? 120 : -120;
        for (int i = 1; i <= g_n; ++i) {
            waitUntil(t);
            send(MOUSEEVENTF_WHEEL, dir);
            if (g_every > 0 && i % g_every == 0 && i < g_n) {
                waitUntil(t + 20);
                send(MOUSEEVENTF_WHEEL, -dir);
                ++g_faults;
            }
            t += 40;
        }
    }
    Sleep(300);
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4 || (strcmp(argv[1], "clicks") && strcmp(argv[1], "up") && strcmp(argv[1], "down"))) {
        printf("usage: inject clicks|up|down <n> <everyK>\n");
        return 2;
    }
    g_mode = argv[1];
    g_n = atoi(argv[2]);
    g_every = atoi(argv[3]);
    if (g_n < 1 || g_n > 2000) { printf("n out of range\n"); return 2; }

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_origin);
    timeBeginPeriod(1);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = &wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"fly-by-wire-inject";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    POINT p;
    GetCursorPos(&p);
    const int size = 280;
    HWND previous = GetForegroundWindow();
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"fly-by-wire scripted mouse",
                             WS_POPUP | WS_VISIBLE, p.x - size / 2, p.y - size / 2, size, size,
                             nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) { printf("could not create the window\n"); return 2; }
    UpdateWindow(g_hwnd);

    RECT old{};
    GetClipCursor(&old);
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    InflateRect(&rc, -40, -40);
    ClipCursor(&rc);

    HANDLE thread = CreateThread(nullptr, 0, &driver, nullptr, 0, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ClipCursor(&old);
    WaitForSingleObject(thread, 2000);
    CloseHandle(thread);
    if (previous) SetForegroundWindow(previous);
    timeEndPeriod(1);
    printf("sent %s: %d with %d fault(s)\n", g_mode, g_n, g_faults);
    return 0;
}
