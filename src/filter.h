// filter.h : the filtering rules. No Windows dependency, so the rules can be
// unit tested by feeding them event sequences with made-up clock values.
//
// Three rules, each of which can be switched off:
//
//   C1  re-press guard    A press that follows the release of the same button
//                         sooner than bounceMs is the switch re-closing by
//                         itself. The press and its release are dropped.
//
//   C2  hold bridge       A release is held back for holdMs. If the button is
//                         pressed again inside that time the switch only
//                         opened for an instant, so neither event is shown to
//                         applications and the button stays down for them.
//                         Costs holdMs of delay on every release.
//
//   W1  reversal guard    A wheel tick against the current direction that
//                         arrives sooner than reverseMs after the last tick is
//                         held back. It only counts if a second tick agrees
//                         with it within confirmMs; otherwise it is dropped.
//
// All times are milliseconds on one monotonic clock.
//
// Contract: before handing in an event with time t, the caller runs tick()
// until it returns false for that same t. That way anything whose time ran out
// is settled first and the event is judged against a clean state.
#pragma once
#include <cstdint>
#include <limits>

namespace fbw {

constexpr int kButtons = 5;
enum Button : int { Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4 };

enum class Verdict : uint8_t {
    Passed = 0,          // delivered unchanged
    BouncePress,         // C1: press too soon after a release, dropped
    BounceRelease,       // C1: the release that belongs to a dropped press
    HeldRelease,         // C2: release held back, fate not known yet
    BridgedPress,        // C2: re-press that cancelled a held release, dropped
    WheelHeld,           // W1: reversed tick held back, fate not known yet
};

// What became of an event that was held back.
enum class Settled : uint8_t {
    None = 0,
    ReleaseEmitted,      // C2: the held release was sent late
    ReleaseBridged,      // C2: the held release was cancelled by a re-press
    WheelConfirmed,      // W1: the held tick was sent late
    WheelDropped,        // W1: the held tick was never confirmed
};

struct Config {
    bool   clickGuard = true;
    double bounceMs[kButtons] = {40, 40, 40, 40, 40};
    bool   holdGuard = false;
    double holdMs = 25;
    bool   wheelGuard = true;
    double reverseMs = 150;
    double confirmMs = 200;
};

// Something the caller has to send into the system itself.
struct Emit {
    enum Kind : uint8_t { ButtonUp, Wheel } kind = ButtonUp;
    int button = 0;      // ButtonUp
    int delta = 0;       // Wheel
};

struct Outcome {
    bool    block = false;                // swallow the incoming event
    Verdict verdict = Verdict::Passed;
    double  gapMs = -1;                   // the gap the rule looked at, -1 if none
    Settled settled = Settled::None;      // an earlier held event was decided
    bool    hasEmit = false;              // send `emit` before letting this one through
    Emit    emit;
};

class Filter {
public:
    void configure(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }

    Outcome button(int b, bool down, double t) {
        Outcome o;
        if (b < 0 || b >= kButtons) return o;
        Btn& s = btn_[b];

        if (down) {
            if (s.hasUp) o.gapMs = t - s.lastUp;
            if (s.pendingUp) {
                // C2: the app still believes the button is down. Keep it that way.
                s.pendingUp = false;
                o.block = true;
                o.verdict = Verdict::BridgedPress;
                o.settled = Settled::ReleaseBridged;
            } else if (s.appDown) {
                // A second press without a release in between. Not ours to judge.
                s.swallow = false;
            } else if (cfg_.clickGuard && s.hasUp && (t - s.lastUp) < cfg_.bounceMs[b]) {
                s.swallow = true;
                o.block = true;
                o.verdict = Verdict::BouncePress;
            } else {
                s.appDown = true;
                s.swallow = false;
            }
            s.lastDown = t;
            s.hasDown = true;
            return o;
        }

        if (s.hasDown) o.gapMs = t - s.lastDown;
        s.lastUp = t;
        s.hasUp = true;
        if (s.swallow) {
            s.swallow = false;
            o.block = true;
            o.verdict = Verdict::BounceRelease;
        } else if (!s.appDown) {
            // Release of a press the app never saw from us (tool started mid press).
        } else if (cfg_.holdGuard && cfg_.holdMs > 0) {
            s.pendingUp = true;
            s.upDeadline = t + cfg_.holdMs;
            o.block = true;
            o.verdict = Verdict::HeldRelease;
        } else {
            s.appDown = false;
        }
        return o;
    }

    Outcome wheel(int delta, double t) {
        Outcome o;
        if (delta == 0) return o;
        const int dir = delta > 0 ? 1 : -1;
        if (hasTick_) o.gapMs = t - lastAccept_;

        if (!cfg_.wheelGuard || !hasTick_) {
            accept(dir, t);
            return o;
        }
        if (dir == lastDir_) {
            if (held_) {
                // The stray tick stood alone between two ticks of the real direction.
                held_ = false;
                o.settled = Settled::WheelDropped;
            }
            accept(dir, t);
            return o;
        }
        if (held_) {
            // Second tick in the new direction: the reversal is real. Send the first too.
            held_ = false;
            o.settled = Settled::WheelConfirmed;
            o.hasEmit = true;
            o.emit.kind = Emit::Wheel;
            o.emit.delta = heldDelta_;
            accept(dir, t);
            return o;
        }
        if ((t - lastAccept_) < cfg_.reverseMs) {
            held_ = true;
            heldDelta_ = delta;
            heldAt_ = t;
            o.block = true;
            o.verdict = Verdict::WheelHeld;
            return o;
        }
        accept(dir, t);
        return o;
    }

    // Time has passed. Returns true if something was settled; `emit` is set
    // when a held release now has to be sent. Call again until it returns false.
    bool tick(double t, Settled* settled, Emit* emit, bool* hasEmit) {
        *hasEmit = false;
        for (int b = 0; b < kButtons; ++b) {
            Btn& s = btn_[b];
            if (s.pendingUp && t >= s.upDeadline) {
                s.pendingUp = false;
                s.appDown = false;
                *settled = Settled::ReleaseEmitted;
                emit->kind = Emit::ButtonUp;
                emit->button = b;
                *hasEmit = true;
                return true;
            }
        }
        if (held_ && t >= heldAt_ + cfg_.confirmMs) {
            held_ = false;
            *settled = Settled::WheelDropped;
            return true;
        }
        return false;
    }

    double nextDeadline() const {
        double d = std::numeric_limits<double>::infinity();
        for (int b = 0; b < kButtons; ++b)
            if (btn_[b].pendingUp && btn_[b].upDeadline < d) d = btn_[b].upDeadline;
        if (held_ && heldAt_ + cfg_.confirmMs < d) d = heldAt_ + cfg_.confirmMs;
        return d;
    }

    // Let go of everything that is being held back, for when the guard is
    // switched off or the program exits. Held releases are sent so no button
    // stays down for applications; a held wheel tick is simply forgotten.
    bool flush(Emit* emit) {
        for (int b = 0; b < kButtons; ++b) {
            Btn& s = btn_[b];
            if (s.pendingUp) {
                s.pendingUp = false;
                s.appDown = false;
                emit->kind = Emit::ButtonUp;
                emit->button = b;
                return true;
            }
        }
        held_ = false;
        return false;
    }

    // True while a button press is hidden from applications or a release is
    // being held. The caller must not replace the hook in that state.
    bool busy() const {
        for (int b = 0; b < kButtons; ++b)
            if (btn_[b].pendingUp || btn_[b].swallow) return true;
        return held_;
    }

private:
    struct Btn {
        bool   appDown = false;     // what applications believe
        bool   swallow = false;     // the current physical press is hidden from them
        bool   pendingUp = false;   // a release is being held back
        double upDeadline = 0;
        bool   hasUp = false, hasDown = false;
        double lastUp = 0, lastDown = 0;
    };

    void accept(int dir, double t) {
        lastDir_ = dir;
        lastAccept_ = t;
        hasTick_ = true;
    }

    Config cfg_;
    Btn    btn_[kButtons];
    bool   hasTick_ = false;
    int    lastDir_ = 0;
    double lastAccept_ = 0;
    bool   held_ = false;
    int    heldDelta_ = 0;
    double heldAt_ = 0;
};

}  // namespace fbw
