// whichmouse.h : finds out which mouse an event came from. No Windows
// dependency, so it can be tested with made-up records.
//
// The low level hook is not told which device an event came from. Raw input
// is: each raw record names its device, and Windows queues the raw record of
// an event before it calls the hook for that event. So the caller hands every
// raw mouse record to seen(), and asks match() once per hook event.
//
// Input that programs inject (this program's own held ticks included) also
// produces raw records, but with no device. Those say nothing about any mouse
// and must never be matched to a real event: on 2026-11-14 exactly that let
// 12 of 63 wrong wheel ticks slip past the rules, each one the tick after a
// tick the guard had sent itself.
#pragma once
#include <cstdint>

namespace fbw {

// The raw record of an event is in the queue before the hook is called for
// it. One older than this belongs to an event the hook never saw.
constexpr double kRawFreshMs = 200;
// If no raw record matches, the event is put down to the mouse that last sent
// anything, as long as that was this recent. One hand, one mouse at a time.
constexpr double kLastMouseMs = 500;

enum DeviceHow : uint8_t { kDevUnknown = 0, kDevExact = 1, kDevLastActive = 2 };

class MouseMatcher {
public:
    // A raw mouse record arrived. `device` is the number of its mouse, 0 when
    // the record has no device (injected input). `flags` are the button and
    // wheel changes it carries, 0 for plain movement.
    void seen(uint8_t device, uint16_t flags, double t) {
        if (!device) return;
        last_ = device;
        lastAt_ = t;
        if (!flags) return;
        Slot& s = slots_[next_];
        next_ = (next_ + 1) % kKeep;
        s.device = device;
        s.flags = flags;
        s.t = t;
    }

    // The mouse behind a hook event that carries the change `flag`: the oldest
    // fresh raw record with that change, which is then used up for it. Failing
    // that, the mouse that was last active. Failing that, 0.
    uint8_t match(uint16_t flag, double now, uint8_t* how) {
        int best = -1;
        for (int i = 0; i < kKeep; ++i) {
            const Slot& s = slots_[i];
            if (!(s.flags & flag) || now - s.t > kRawFreshMs) continue;
            if (best < 0 || s.t < slots_[best].t) best = i;
        }
        if (best >= 0) {
            slots_[best].flags = static_cast<uint16_t>(slots_[best].flags & ~flag);
            *how = kDevExact;
            return slots_[best].device;
        }
        if (last_ && now - lastAt_ <= kLastMouseMs) {
            *how = kDevLastActive;
            return last_;
        }
        *how = kDevUnknown;
        return 0;
    }

private:
    static constexpr int kKeep = 32;
    struct Slot { uint8_t device = 0; uint16_t flags = 0; double t = 0; };
    Slot    slots_[kKeep];
    int     next_ = 0;
    uint8_t last_ = 0;
    double  lastAt_ = 0;
};

}  // namespace fbw
