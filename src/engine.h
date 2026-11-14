// engine.h : the low level mouse hook, the thread that owns the filter, and
// the thread that sends the few events the filter produces itself.
//
// Threading, and why:
//   hook thread     runs the hook callback and all filter state, so the rules
//                   need no lock. Highest priority: every mouse event in the
//                   system waits for this callback to return.
//   emitter thread  calls SendInput. Kept off the hook thread because the
//                   system calls the hook again for what we send.
// Records go out through a lock-free ring; the reader is whoever owns the UI.
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

#include "filter.h"
#include "ring.h"

namespace fbw {

// dwExtraInfo of every event fly-by-wire sends, so the hook can tell them apart.
constexpr ULONG_PTR kMarker = 0x46425731;   // "FBW1"

enum RecKind : uint8_t { kButton = 0, kWheel = 1, kHWheel = 2, kSettled = 3 };
enum RecSource : uint8_t { kHardware = 0, kOtherProgram = 1, kOurs = 2 };

struct Rec {
    double  t = 0;            // ms since the engine started
    uint8_t kind = kButton;
    uint8_t button = 0;
    uint8_t down = 0;
    int16_t delta = 0;
    uint8_t source = kHardware;
    uint8_t blocked = 0;
    uint8_t verdict = 0;      // fbw::Verdict
    uint8_t settled = 0;      // fbw::Settled
    float   gap = -1;         // the gap the rule looked at, ms
};

class Engine {
public:
    // Self test only: treat events injected by other programs as hardware.
    bool treatInjectedAsReal = false;

    std::atomic<uint64_t> hookCalls{0};       // every callback, moves included
    std::atomic<uint64_t> lostRecords{0};     // ring was full
    std::atomic<uint32_t> rehooks{0};
    std::atomic<uint32_t> sendFailures{0};

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

    // Safe from any thread. The hook thread applies it between events.
    void setConfig(const Config& rules, bool enabled) {
        auto* p = new Pending{rules, enabled};
        if (!PostThreadMessageW(hookThreadId_, kMsgConfig, 0, reinterpret_cast<LPARAM>(p))) delete p;
    }

    // Ask the hook thread to install the hook again (see watchdog in main.cpp).
    void rehook() { PostThreadMessageW(hookThreadId_, kMsgRehook, 0, 0); }

    bool   popRecord(Rec* r) { return records_.pop(r); }
    HANDLE recordEvent() const { return recordEvent_; }

    double nowMs() const {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart - origin_.QuadPart) * 1000.0 / static_cast<double>(freq_.QuadPart);
    }

private:
    struct Pending { Config rules; bool enabled; };
    static constexpr UINT kMsgConfig = WM_APP + 1;
    static constexpr UINT kMsgRehook = WM_APP + 2;

    static DWORD WINAPI hookEntry(void* p) { return static_cast<Engine*>(p)->hookLoop(); }
    static DWORD WINAPI emitEntry(void* p) { return static_cast<Engine*>(p)->emitLoop(); }

    void push(const Rec& r) {
        if (!records_.push(r)) lostRecords.fetch_add(1, std::memory_order_relaxed);
        SetEvent(recordEvent_);
    }

    void queueEmit(const Emit& e) {
        emits_.push(e);
        SetEvent(emitEvent_);
    }

    void applyEffective() {
        Config c = rules_;
        if (!enabled_) { c.clickGuard = false; c.holdGuard = false; c.wheelGuard = false; }
        filter_.configure(c);
    }

    void flushHeld() {
        Emit e;
        while (filter_.flush(&e)) queueEmit(e);
    }

    void runTicks() {
        Settled settled = Settled::None;
        Emit e;
        bool hasEmit = false;
        while (filter_.tick(nowMs(), &settled, &e, &hasEmit)) {
            if (hasEmit) queueEmit(e);
            Rec r;
            r.t = nowMs();
            r.kind = kSettled;
            r.settled = static_cast<uint8_t>(settled);
            if (hasEmit) r.button = static_cast<uint8_t>(e.button);
            push(r);
        }
    }

    DWORD hookLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // creates the message queue
        applyEffective();
        hook_.store(SetWindowsHookExW(WH_MOUSE_LL, &Engine::hookProc, GetModuleHandleW(nullptr), 0));
        SetEvent(ready_);
        if (!hook_.load()) return 1;

        // High resolution timer where the system has one (Windows 10 1803 on),
        // so a 25 ms hold is 25 ms and not rounded up to the 15.6 ms clock tick.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

        bool running = true;
        while (running) {
            const double due = filter_.nextDeadline();
            if (due != std::numeric_limits<double>::infinity()) {
                double wait = due - nowMs();
                if (wait < 0.05) wait = 0.05;
                LARGE_INTEGER li;
                li.QuadPart = -static_cast<LONGLONG>(wait * 10000.0);
                SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
            } else {
                CancelWaitableTimer(timer);
            }
            MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            // The hook callback runs inside PeekMessage, on this thread.
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    running = false;
                } else if (msg.message == kMsgConfig) {
                    auto* p = reinterpret_cast<Pending*>(msg.lParam);
                    flushHeld();
                    rules_ = p->rules;
                    enabled_ = p->enabled;
                    applyEffective();
                    delete p;
                } else if (msg.message == kMsgRehook) {
                    // Never swap the hook while an event is hidden or held: the
                    // matching event must reach the same filter state.
                    if (!filter_.busy()) {
                        HHOOK fresh = SetWindowsHookExW(WH_MOUSE_LL, &Engine::hookProc, GetModuleHandleW(nullptr), 0);
                        if (fresh) {
                            UnhookWindowsHookEx(hook_.load());
                            hook_.store(fresh);
                            rehooks.fetch_add(1);
                        }
                    }
                }
            }
            runTicks();
        }

        flushHeld();
        UnhookWindowsHookEx(hook_.load());
        hook_.store(nullptr);
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
        r.button = static_cast<uint8_t>(button);
        r.down = down ? 1 : 0;
        r.delta = static_cast<int16_t>(delta);

        // What we sent ourselves, and what other programs inject (GG macros,
        // remote control), is recorded but never judged.
        if (m.dwExtraInfo == kMarker) {
            r.source = kOurs;
            e->push(r);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        const bool injected = (m.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0;
        if ((injected && !e->treatInjectedAsReal) || r.kind == kHWheel) {
            if (injected) r.source = kOtherProgram;
            e->push(r);
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // Settle whatever ran out of time before this event, so a release whose
        // hold has expired is sent first and cannot be bridged by a late press.
        e->runTicks();
        const Outcome o = r.kind == kWheel ? e->filter_.wheel(delta, r.t) : e->filter_.button(button, down, r.t);
        r.blocked = o.block ? 1 : 0;
        r.verdict = static_cast<uint8_t>(o.verdict);
        r.settled = static_cast<uint8_t>(o.settled);
        r.gap = static_cast<float>(o.gapMs);
        if (o.hasEmit) e->queueEmit(o.emit);
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

    // Touched only by the hook thread.
    Filter filter_;
    Config rules_;
    bool   enabled_ = false;

    Ring<Rec, 4096> records_;
    Ring<Emit, 256> emits_;
};

}  // namespace fbw
