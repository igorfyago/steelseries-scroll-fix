// engine.h : the low level mouse hook, the thread that owns the filter, and
// the thread that sends the wheel ticks the filter produces itself.
//
// Threading, and why:
//   hook thread     runs the hook callback and all filter state, so the rules
//                   need no lock. Highest priority: every mouse event in the
//                   system waits for this callback to return. It also sends
//                   the one kind of event whose order matters: a button
//                   release that was held back must reach applications before
//                   the next press of that button does.
//   emitter thread  sends wheel ticks that were held back and then released.
//                   Their order among ticks of the same direction does not
//                   matter, and keeping them off the hook thread means a slow
//                   hook in some other program cannot hold ours up.
// Records go out through a lock-free ring; the reader is whoever owns the UI.
//
// Which mouse: the hook itself is not told which device an event came from.
// Raw input is: every raw record names its device, and Windows queues the raw
// record of an event before it calls the hook for it (measured on this PC: 35
// of 35 clicks and wheel ticks). So the hook thread also receives raw mouse
// input, and each callback looks up the device of its own event there. Only
// events of the guarded mouse are judged; the others pass untouched.
//
// The guard fails open. Whenever its view of the mouse may be stale (it was
// switched off, the hook had to be put back, a callback ran long), it lets go
// of everything it holds and stops hiding anything, so the worst case is the
// unfiltered mouse and never a button that applications see stuck down.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cwchar>
#include <mutex>

#include "filter.h"
#include "ring.h"

namespace fbw {

// dwExtraInfo of every event fly-by-wire sends, so the hook can tell them apart.
constexpr ULONG_PTR kMarker = 0x46425731;   // "FBW1"

// Windows stops waiting for a hook callback after a while (300 ms unless the
// LowLevelHooksTimeout setting says otherwise, never more than 1000) and
// delivers the event whatever the callback answers later. A verdict that took
// this long is therefore not trusted: the event passes and the filter lets go.
constexpr double kStallMs = 100;
// The same for a callback that started this late after Windows stamped the event.
constexpr int kLateMs = 250;
// Own time above this is counted, so the page can show the hook stays fast.
constexpr double kSlowMs = 2;
// The raw record of an event is in the queue before the hook is called for
// it. One older than this belongs to an event the hook never saw.
constexpr double kRawFreshMs = 200;
// If no raw record matches, the event is put down to the mouse that last sent
// anything, as long as that was this recent. One hand, one mouse at a time.
constexpr double kLastMouseMs = 500;

enum RecKind : uint8_t { kButton = 0, kWheel = 1, kHWheel = 2, kSettled = 3 };
enum RecSource : uint8_t { kHardware = 0, kOtherProgram = 1, kOurs = 2 };
enum DeviceHow : uint8_t { kDevUnknown = 0, kDevExact = 1, kDevLastActive = 2 };

struct Rec {
    double   t = 0;           // ms since the engine started, read when the callback began
    uint32_t sys = 0;         // the stamp Windows gave the event (its coarse clock)
    int32_t  lag = -1;        // coarse clock now minus that stamp: how late the callback began
    uint8_t  kind = kButton;
    uint8_t  button = 0;
    uint8_t  down = 0;
    int16_t  delta = 0;
    uint8_t  source = kHardware;
    uint8_t  blocked = 0;
    uint8_t  verdict = 0;     // fbw::Verdict
    uint8_t  settled = 0;     // fbw::Settled
    uint8_t  flags = 0;       // LLMHF_* as Windows reported them
    uint8_t  vetoed = 0;      // the fine clock said block, the coarse clock said no
    uint8_t  stalled = 0;     // verdict not trusted (see kStallMs, kLateMs): passed
    uint8_t  device = 0;      // which mouse, as a number from Engine::device(); 0 = not known
    uint8_t  deviceHow = 0;   // DeviceHow: how that was found out
    uint8_t  judged = 0;      // this event went through the rules (it came from the guarded mouse)
    float    gap = -1;        // the gap the rule looked at, ms
};

// One mouse as Windows names it. Numbers start at 1 and never change while the
// program runs; a mouse that is unplugged and plugged in again gets a new one.
struct Device {
    uint16_t vid = 0, pid = 0;      // USB vendor and product id, 0 if the name has none
    wchar_t  path[260] = {};
};

class Engine {
public:
    static constexpr int kMaxDevices = 16;

    // Self test only: treat events injected by other programs as hardware of
    // the guarded mouse.
    bool treatInjectedAsReal = false;
    // Self test only: wake up this much late for a held release, so the case
    // where the next press gets to the hook before the timer does can be
    // produced on demand.
    std::atomic<double> testTimerLateMs{0};

    std::atomic<uint64_t> hookCalls{0};       // every callback, moves included
    std::atomic<uint64_t> lostRecords{0};     // ring was full
    std::atomic<uint32_t> rehooks{0};
    std::atomic<uint32_t> sendFailures{0};
    std::atomic<uint32_t> slowCalls{0};       // button or wheel callbacks over kSlowMs of own time
    std::atomic<uint32_t> stalls{0};          // callbacks whose verdict was not trusted
    std::atomic<uint32_t> maxOwnUs{0};        // longest own time of a button or wheel callback
    std::atomic<uint32_t> unknownDevice{0};   // mouse events whose device could not be found out
    std::atomic<bool>     rawInput{false};    // raw input is registered: devices can be told apart

    bool start() {
        if (self_) return false;
        self_ = this;
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&origin_);
        recordEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        emitEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        emitThread_ = CreateThread(nullptr, 0, &Engine::emitEntry, this, 0, nullptr);
        hookThread_ = CreateThread(nullptr, 0, &Engine::hookEntry, this, 0, &hookThreadId_);
        WaitForSingleObject(ready_, 5000);
        return hook_.load() != nullptr;
    }

    void stop() {
        if (!self_) return;
        if (hookThread_) {
            PostThreadMessageW(hookThreadId_, WM_QUIT, 0, 0);
            WaitForSingleObject(hookThread_, 3000);
            CloseHandle(hookThread_);
            hookThread_ = nullptr;
        }
        quitEmit_.store(true);
        SetEvent(emitEvent_);
        if (emitThread_) {
            WaitForSingleObject(emitThread_, 3000);
            CloseHandle(emitThread_);
            emitThread_ = nullptr;
        }
        self_ = nullptr;
    }

    // Safe from any thread. The hook thread applies it between events. Going
    // off does not wait for that: the very next callback already passes everything.
    // vid and pid name the one mouse to guard; 0 and 0 mean every mouse.
    void setConfig(const Config& rules, bool enabled, uint16_t vid = 0, uint16_t pid = 0) {
        {
            std::lock_guard<std::mutex> lock(configMu_);
            offNow_.store(!enabled, std::memory_order_relaxed);
            Pending p;
            p.rules = rules;
            p.enabled = enabled;
            p.vid = vid;
            p.pid = pid;
            configs_.push(p);       // full only if the hook thread is gone
        }
        PostThreadMessageW(hookThreadId_, kMsgWake, 0, 0);
    }

    // Ask the hook thread to install the hook again (see watchdog in main.cpp).
    void rehook() { PostThreadMessageW(hookThreadId_, kMsgRehook, 0, 0); }

    bool   popRecord(Rec* r) { return records_.pop(r); }
    HANDLE recordEvent() const { return recordEvent_; }

    // Mice seen so far, numbered from 1. Safe from any thread: an entry never
    // changes once the count includes it.
    int deviceCount() const { return deviceCount_.load(std::memory_order_acquire); }
    bool device(int number, Device* out) const {
        if (number < 1 || number > deviceCount()) return false;
        *out = devices_[number - 1].info;
        return true;
    }

    double nowMs() const {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart - origin_.QuadPart) * 1000.0 / static_cast<double>(freq_.QuadPart);
    }

private:
    struct Pending { Config rules; bool enabled = false; uint16_t vid = 0, pid = 0; };
    struct Slot { HANDLE handle = nullptr; Device info; };
    struct RawSeen { uint8_t device = 0; USHORT flags = 0; double t = 0; };
    static constexpr UINT kMsgWake = WM_APP + 1;
    static constexpr UINT kMsgRehook = WM_APP + 2;
    static constexpr int kRawKeep = 32;

    static DWORD WINAPI hookEntry(void* p) { return static_cast<Engine*>(p)->hookLoop(); }
    static DWORD WINAPI emitEntry(void* p) { return static_cast<Engine*>(p)->emitLoop(); }

    void push(const Rec& r) {
        if (!records_.push(r)) lostRecords.fetch_add(1, std::memory_order_relaxed);
        SetEvent(recordEvent_);
    }

    // Hook thread. A release goes out at once, from here, so that it is in the
    // system's input stream before the event this thread is about to let through.
    // A wheel tick is handed to the emitter thread.
    void emit(const Emit& e) {
        if (e.kind == Emit::ButtonUp) {
            send(e);
        } else {
            emits_.push(e);
            SetEvent(emitEvent_);
        }
    }

    void applyEffective() {
        Config c = rules_;
        if (!enabled_) {
            c.clickGuard = false;
            c.wheelGuard = false;
        }
        // Holding a release means sending it ourselves later. With the buttons
        // swapped in Windows settings that path is untested, so it stays off.
        if (!enabled_ || GetSystemMetrics(SM_SWAPBUTTON)) {
            for (double& h : c.holdMs) h = 0;
        }
        filter_.configure(c);
    }

    void flushHeld() {
        Emit e;
        while (filter_.flush(&e)) emit(e);
    }

    // Nothing held, nothing hidden: from here every event passes until the
    // rules are applied to fresh input again.
    void letGo() {
        flushHeld();
        filter_.forgetHidden();
    }

    void applyPending() {
        Pending p, last;
        bool any = false;
        while (configs_.pop(&p)) { last = p; any = true; }
        if (!any) return;
        flushHeld();
        // Another mouse is guarded from now on: what the filter knows is about the old one.
        if (last.vid != scopeVid_ || last.pid != scopePid_) filter_.forgetHidden();
        rules_ = last.rules;
        enabled_ = last.enabled;
        scopeVid_ = last.vid;
        scopePid_ = last.pid;
        applyEffective();
        if (!enabled_) filter_.forgetHidden();
    }

    void runTicks() {
        Settled settled = Settled::None;
        Emit e;
        bool hasEmit = false;
        while (filter_.tick(nowMs(), &settled, &e, &hasEmit)) {
            Rec r;
            r.t = nowMs();
            r.kind = kSettled;
            r.settled = static_cast<uint8_t>(settled);
            r.judged = 1;
            if (hasEmit && e.kind == Emit::ButtonUp) r.button = static_cast<uint8_t>(e.button);
            push(r);
            if (hasEmit) emit(e);      // the filter state is already updated: a release sent here re-enters the hook
        }
    }

    // ---- which mouse --------------------------------------------------------

    static LRESULT CALLBACK rawWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(h, msg, wp, lp); }

    void startRawInput() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &Engine::rawWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"fly-by-wire-raw";
        RegisterClassW(&wc);        // fails harmlessly if it is registered already
        rawWindow_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!rawWindow_) return;
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;     // generic desktop
        rid.usUsage = 0x02;         // mouse
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = rawWindow_;
        rawInput.store(RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE);
    }

    void stopRawInput() {
        if (rawInput.load()) {
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = 0x01;
            rid.usUsage = 0x02;
            rid.dwFlags = RIDEV_REMOVE;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            rawInput.store(false);
        }
        if (rawWindow_) { DestroyWindow(rawWindow_); rawWindow_ = nullptr; }
    }

    static uint16_t hexAfter(const wchar_t* path, const wchar_t* key) {
        const wchar_t* p = path;
        const size_t n = wcslen(key);
        for (; *p; ++p) {
            if (_wcsnicmp(p, key, n) != 0) continue;
            unsigned v = 0;
            int digits = 0;
            for (const wchar_t* d = p + n; digits < 4; ++d, ++digits) {
                const wchar_t c = *d;
                unsigned x;
                if (c >= L'0' && c <= L'9') x = static_cast<unsigned>(c - L'0');
                else if (c >= L'a' && c <= L'f') x = static_cast<unsigned>(c - L'a') + 10;
                else if (c >= L'A' && c <= L'F') x = static_cast<unsigned>(c - L'A') + 10;
                else break;
                v = v * 16 + x;
            }
            if (digits == 4) return static_cast<uint16_t>(v);
        }
        return 0;
    }

    // The number of a raw input device handle, adding it to the table the
    // first time it is seen. 0 when there is no handle (injected input) or the
    // table is full.
    uint8_t deviceNumber(HANDLE handle) {
        if (!handle) return 0;
        const int n = deviceCount_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) if (devices_[i].handle == handle) return static_cast<uint8_t>(i + 1);
        if (n >= kMaxDevices) return 0;
        Slot& s = devices_[n];
        s.handle = handle;
        UINT chars = static_cast<UINT>(sizeof(s.info.path) / sizeof(wchar_t)) - 1;
        if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, s.info.path, &chars) == static_cast<UINT>(-1)) s.info.path[0] = 0;
        s.info.path[sizeof(s.info.path) / sizeof(wchar_t) - 1] = 0;
        s.info.vid = hexAfter(s.info.path, L"VID_");
        s.info.pid = hexAfter(s.info.path, L"PID_");
        deviceCount_.store(n + 1, std::memory_order_release);
        return static_cast<uint8_t>(n + 1);
    }

    // Reads every raw mouse record queued for this thread and remembers the
    // ones that carry a button or wheel change.
    void drainRaw() {
        if (!rawInput.load(std::memory_order_relaxed)) return;
        for (;;) {
            UINT size = sizeof(rawBuf_);
            const UINT n = GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(rawBuf_), &size, sizeof(RAWINPUTHEADER));
            if (n == 0 || n == static_cast<UINT>(-1)) return;
            const double now = nowMs();
            const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(rawBuf_);
            for (UINT i = 0; i < n; ++i) {
                if (ri->header.dwType == RIM_TYPEMOUSE) {
                    const uint8_t dev = deviceNumber(ri->header.hDevice);
                    if (dev) { lastMouse_ = dev; lastMouseAt_ = now; }
                    if (ri->data.mouse.usButtonFlags) {
                        RawSeen& slot = rawSeen_[rawNext_];
                        rawNext_ = (rawNext_ + 1) % kRawKeep;
                        slot.device = dev;
                        slot.flags = ri->data.mouse.usButtonFlags;
                        slot.t = now;
                    }
                }
                // next record: each one is padded to a multiple of 8 bytes
                ri = reinterpret_cast<const RAWINPUT*>((reinterpret_cast<ULONG_PTR>(ri) + ri->header.dwSize + 7) & ~static_cast<ULONG_PTR>(7));
            }
        }
    }

    // Finds the mouse an event came from: the oldest fresh raw record that
    // carries the same change, else the mouse that was last active.
    uint8_t whichMouse(USHORT rawFlag, double now, uint8_t* how) {
        drainRaw();
        int best = -1;
        for (int i = 0; i < kRawKeep; ++i) {
            const RawSeen& s = rawSeen_[i];
            if (!(s.flags & rawFlag) || now - s.t > kRawFreshMs) continue;
            if (best < 0 || s.t < rawSeen_[best].t) best = i;
        }
        if (best >= 0) {
            rawSeen_[best].flags = static_cast<USHORT>(rawSeen_[best].flags & ~rawFlag);
            *how = rawSeen_[best].device ? kDevExact : kDevUnknown;
            return rawSeen_[best].device;
        }
        if (lastMouse_ && now - lastMouseAt_ <= kLastMouseMs) {
            *how = kDevLastActive;
            return lastMouse_;
        }
        *how = kDevUnknown;
        return 0;
    }

    bool guarded(uint8_t device) const {
        if (!scopeVid_ && !scopePid_) return true;                 // every mouse
        if (!device) return false;                                 // not known whose it is: leave it alone
        const Device& d = devices_[device - 1].info;
        return d.vid == scopeVid_ && d.pid == scopePid_;
    }

    static USHORT rawFlagFor(WPARAM wp, const MSLLHOOKSTRUCT& m) {
        switch (wp) {
            case WM_LBUTTONDOWN: return RI_MOUSE_LEFT_BUTTON_DOWN;
            case WM_LBUTTONUP:   return RI_MOUSE_LEFT_BUTTON_UP;
            case WM_RBUTTONDOWN: return RI_MOUSE_RIGHT_BUTTON_DOWN;
            case WM_RBUTTONUP:   return RI_MOUSE_RIGHT_BUTTON_UP;
            case WM_MBUTTONDOWN: return RI_MOUSE_MIDDLE_BUTTON_DOWN;
            case WM_MBUTTONUP:   return RI_MOUSE_MIDDLE_BUTTON_UP;
            case WM_XBUTTONDOWN: return HIWORD(m.mouseData) == XBUTTON1 ? RI_MOUSE_BUTTON_4_DOWN : RI_MOUSE_BUTTON_5_DOWN;
            case WM_XBUTTONUP:   return HIWORD(m.mouseData) == XBUTTON1 ? RI_MOUSE_BUTTON_4_UP : RI_MOUSE_BUTTON_5_UP;
            case WM_MOUSEWHEEL:  return RI_MOUSE_WHEEL;
            case WM_MOUSEHWHEEL: return 0x0800;                    // RI_MOUSE_HWHEEL
        }
        return 0;
    }

    // ---- threads ------------------------------------------------------------

    void handleThreadMessage(const MSG& msg, bool* running) {
        if (msg.message == WM_QUIT) {
            *running = false;
        } else if (msg.message == kMsgRehook) {
            // The hook may have been gone for a while, so what the filter
            // believes about the buttons may be wrong. Let go first, then put
            // a fresh hook in and take the old one out.
            letGo();
            HHOOK fresh = SetWindowsHookExW(WH_MOUSE_LL, &Engine::hookProc, GetModuleHandleW(nullptr), 0);
            if (fresh) {
                UnhookWindowsHookEx(hook_.load());
                hook_.store(fresh);
                rehooks.fetch_add(1);
            }
        }
    }

    DWORD hookLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // creates the message queue
        applyEffective();
        startRawInput();
        hook_.store(SetWindowsHookExW(WH_MOUSE_LL, &Engine::hookProc, GetModuleHandleW(nullptr), 0));
        SetEvent(ready_);
        if (!hook_.load()) { stopRawInput(); return 1; }

        // High resolution timer where the system has one (Windows 10 1803 on),
        // so a 12 ms hold is 12 ms and not rounded up to the 15.6 ms clock tick.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

        bool running = true;
        while (running) {
            const double due = filter_.nextDeadline();
            if (due != std::numeric_limits<double>::infinity()) {
                double wait = due - nowMs() + testTimerLateMs.load(std::memory_order_relaxed);
                if (wait < 0.05) wait = 0.05;
                LARGE_INTEGER li;
                li.QuadPart = -static_cast<LONGLONG>(wait * 10000.0);
                SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
            } else {
                CancelWaitableTimer(timer);
            }
            MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            // Raw input first, so the queue never fills up with it. Then every
            // other message; WM_INPUT itself is left out of both ranges, its
            // data having been read just now or being read by the next round.
            // The hook callback runs inside PeekMessage, on this thread.
            drainRaw();
            while (PeekMessageW(&msg, nullptr, 0, WM_INPUT - 1, PM_REMOVE)) handleThreadMessage(msg, &running);
            while (PeekMessageW(&msg, nullptr, WM_INPUT + 1, 0xFFFFFFFF, PM_REMOVE)) handleThreadMessage(msg, &running);
            applyPending();
            runTicks();
        }

        flushHeld();
        UnhookWindowsHookEx(hook_.load());
        hook_.store(nullptr);
        stopRawInput();
        CloseHandle(timer);
        return 0;
    }

    DWORD emitLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        for (;;) {
            WaitForSingleObject(emitEvent_, INFINITE);
            Emit e;
            while (emits_.pop(&e)) send(e);
            if (quitEmit_.load()) break;
        }
        return 0;
    }

    void send(const Emit& e) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwExtraInfo = kMarker;
        if (e.kind == Emit::Wheel) {
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = static_cast<DWORD>(e.delta);
        } else {
            switch (e.button) {
                case Left:   in.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
                case Right:  in.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
                case Middle: in.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
                case X1:     in.mi.dwFlags = MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON1; break;
                default:     in.mi.dwFlags = MOUSEEVENTF_XUP; in.mi.mouseData = XBUTTON2; break;
            }
        }
        if (SendInput(1, &in, sizeof(in)) != 1) sendFailures.fetch_add(1);
    }

    static LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp) {
        Engine* e = self_;
        if (code != HC_ACTION || !e) return CallNextHookEx(nullptr, code, wp, lp);
        e->hookCalls.fetch_add(1, std::memory_order_relaxed);
        if (wp == WM_MOUSEMOVE) return CallNextHookEx(nullptr, code, wp, lp);

        const MSLLHOOKSTRUCT& m = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        Rec r;
        int button = 0;
        bool down = false;
        int delta = 0;
        switch (wp) {
            case WM_LBUTTONDOWN: button = Left; down = true; break;
            case WM_LBUTTONUP:   button = Left; break;
            case WM_RBUTTONDOWN: button = Right; down = true; break;
            case WM_RBUTTONUP:   button = Right; break;
            case WM_MBUTTONDOWN: button = Middle; down = true; break;
            case WM_MBUTTONUP:   button = Middle; break;
            case WM_XBUTTONDOWN: button = HIWORD(m.mouseData) == XBUTTON1 ? X1 : X2; down = true; break;
            case WM_XBUTTONUP:   button = HIWORD(m.mouseData) == XBUTTON1 ? X1 : X2; break;
            case WM_MOUSEWHEEL:  r.kind = kWheel; delta = static_cast<short>(HIWORD(m.mouseData)); break;
            case WM_MOUSEHWHEEL: r.kind = kHWheel; delta = static_cast<short>(HIWORD(m.mouseData)); break;
            default: return CallNextHookEx(nullptr, code, wp, lp);
        }
        r.t = e->nowMs();
        r.sys = m.time;
        const uint32_t behind = GetTickCount() - m.time;       // unsigned: fine across the clock wrapping round
        r.lag = behind < 60000u ? static_cast<int32_t>(behind) : -1;   // -1: the sender made the stamp up
        r.flags = static_cast<uint8_t>(m.flags & 0xFF);
        r.button = static_cast<uint8_t>(button);
        r.down = down ? 1 : 0;
        r.delta = static_cast<int16_t>(delta);

        // What we sent ourselves, and what other programs inject (GG macros,
        // remote control), is recorded but never judged.
        const bool injected = (m.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0;
        if (m.dwExtraInfo == kMarker && injected) {
            r.source = kOurs;
            e->push(r);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        const bool asHardware = !injected || e->treatInjectedAsReal;
        if (!asHardware) {
            r.source = kOtherProgram;
            e->push(r);
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // Which mouse sent it. Injected input has no device; in the self test
        // it stands in for the guarded mouse.
        bool mine = true;
        if (!injected) {
            r.device = e->whichMouse(rawFlagFor(wp, m), r.t, &r.deviceHow);
            if (!r.device) e->unknownDevice.fetch_add(1, std::memory_order_relaxed);
            mine = e->guarded(r.device);
        }
        if (!mine || r.kind == kHWheel) {
            e->push(r);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        r.judged = 1;

        // Going off takes effect here, without waiting for the message loop.
        if (e->enabled_ && e->offNow_.load(std::memory_order_relaxed)) {
            e->enabled_ = false;
            e->flushHeld();
            e->applyEffective();
            e->filter_.forgetHidden();
        }

        // Settle whatever ran out of time before this event. A release whose
        // hold has expired is sent right here, before this event is let through.
        e->runTicks();
        Stamp sys;
        sys.known = true;
        sys.ms = m.time;
        const Outcome o = r.kind == kWheel ? e->filter_.wheel(delta, r.t, sys) : e->filter_.button(button, down, r.t, sys);
        r.blocked = o.block ? 1 : 0;
        r.verdict = static_cast<uint8_t>(o.verdict);
        r.settled = static_cast<uint8_t>(o.settled);
        r.vetoed = o.vetoed ? 1 : 0;
        r.gap = static_cast<float>(o.gapMs);
        if (o.hasEmit) e->emit(o.emit);
        if (o.hasEmit2) e->emit(o.emit2);

        const double own = e->nowMs() - r.t;
        const uint32_t ownUs = own > 4000000.0 ? 4000000000u : static_cast<uint32_t>(own * 1000.0);
        if (ownUs > e->maxOwnUs.load(std::memory_order_relaxed)) e->maxOwnUs.store(ownUs, std::memory_order_relaxed);
        if (own > kSlowMs) e->slowCalls.fetch_add(1, std::memory_order_relaxed);
        if (own > kStallMs || r.lag >= kLateMs) {
            // Windows may already have given up on this callback and delivered
            // the event. Do not rely on having blocked it.
            e->stalls.fetch_add(1, std::memory_order_relaxed);
            e->letGo();
            r.stalled = 1;
            r.blocked = 0;
            e->push(r);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        e->push(r);
        return o.block ? 1 : CallNextHookEx(nullptr, code, wp, lp);
    }

    static inline Engine* self_ = nullptr;

    LARGE_INTEGER freq_{}, origin_{};
    std::atomic<HHOOK> hook_{nullptr};
    HANDLE hookThread_ = nullptr, emitThread_ = nullptr;
    DWORD  hookThreadId_ = 0;
    HANDLE recordEvent_ = nullptr, emitEvent_ = nullptr, ready_ = nullptr;
    std::atomic<bool> quitEmit_{false};

    // Settings on their way to the hook thread. Any thread may add one (under
    // the mutex); only the hook thread takes them out, and it never locks.
    std::mutex configMu_;
    Ring<Pending, 16> configs_;
    std::atomic<bool> offNow_{true};

    // Mice. Written by the hook thread only; an entry is complete before the
    // count that makes it visible to other threads is raised.
    Slot devices_[kMaxDevices];
    std::atomic<int> deviceCount_{0};

    // Touched only by the hook thread.
    Filter filter_;
    Config rules_;
    bool   enabled_ = false;
    uint16_t scopeVid_ = 0, scopePid_ = 0;
    HWND   rawWindow_ = nullptr;
    RawSeen rawSeen_[kRawKeep];
    int     rawNext_ = 0;
    uint8_t lastMouse_ = 0;
    double  lastMouseAt_ = 0;
    alignas(8) BYTE rawBuf_[16 * 1024];

    Ring<Rec, 4096> records_;
    Ring<Emit, 256> emits_;
};

}  // namespace fbw
