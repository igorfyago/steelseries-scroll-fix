// filter.h : the filtering rules. No Windows dependency, so the rules can be
// unit tested by feeding them event sequences with made-up clock values.
//
// Three rules, each of which can be switched off:
//
//   C1  re-press guard    A press that follows the release of the same button
//                         sooner than bounceMs is the switch re-closing by
//                         itself. The press and its release are dropped.
//                         Nothing is delayed and nothing is sent.
//
//   C2  hold bridge       A release is held back for holdMs. If the button is
//                         pressed again inside that time the switch only
//                         opened for an instant, so neither event is shown to
//                         applications and the button stays down for them.
//                         Costs holdMs of delay on every release of that
//                         button, so it is set per button and off by default.
//
//   W1  reversal guard    A wheel tick against the stroke that arrives sooner
//                         than reverseMs after the last tick is held back: no
//                         hand turns a wheel round that fast. If the next tick
//                         agrees with it within confirmMs the turn was real and
//                         both are delivered. If the stroke goes on in its old
//                         direction, or nothing follows, the held tick was a
//                         misread step of the encoder: it is delivered turned
//                         round (wheelFlip) or dropped.
//
//   W2  first ticks       The first tick of a new stroke (nothing for reverseMs
//                         or longer) has no direction to be checked against, so
//                         it waits up to firstMs for the second. If they agree
//                         both are delivered. If they disagree one of the two
//                         was misread and neither can say which, so both wait
//                         for a third tick, and the odd one out is turned round
//                         or dropped. Whatever nothing follows is delivered as
//                         it came, firstMs late. 0 switches this off.
//
// Why turning a tick round is sound, measured on the mouse this was built for
// (1270 ticks, 84 of them the wrong way): strokes with a wrong tick have the
// same number of ticks in total as strokes without one, and the gap around a
// wrong tick is two normal spacings. The wrong tick stands where a right one
// is missing. At the start of a stroke the wrong one was the first tick 11
// times and the second 4 times, which is why two ticks cannot settle it.
//
// All times are milliseconds on one monotonic clock, read when the event
// reached us. That clock is fine grained but it runs late whenever our thread
// was kept waiting, and two late readings can make a long gap look short. So
// every event may also carry the stamp Windows gave it on its own coarse clock
// (steps of about 16 ms). The coarse stamp never causes a block. It can only
// veto one: if it says the gap was clearly long, the event passes.
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

// One step of the coarse system clock, rounded up.
constexpr double kCoarseStepMs = 16;

enum class Verdict : uint8_t {
    Passed = 0,          // delivered unchanged
    BouncePress,         // C1: press too soon after a release, dropped
    BounceRelease,       // C1: the release that belongs to a dropped press
    HeldRelease,         // C2: release held back, fate not known yet
    BridgedPress,        // C2: re-press that cancelled a held release, dropped
    WheelHeld,           // W1: reversed tick held back, fate not known yet
    ExtraRelease,        // C2: a second release while one is already held, dropped
    WheelFirst,          // W2: first tick of a stroke held back, fate not known yet
};

// What became of an event that was held back.
enum class Settled : uint8_t {
    None = 0,
    ReleaseEmitted,      // C2: the held release was sent late
    ReleaseBridged,      // C2: the held release was cancelled by a re-press
    WheelConfirmed,      // W1: a tick against the stroke was sent late: the next tick agreed, the turn was real
    WheelDropped,        // the held tick was dropped
    WheelFlipped,        // the held tick was sent turned round: the next tick showed it was misread
    WheelLate,           // W2: nothing followed the first tick, so it was sent as it came
    WheelStarted,        // W2: the second tick agreed with the first, which was then sent
};

struct Config {
    bool   clickGuard = true;
    double bounceMs[kButtons] = {20, 20, 20, 20, 20};   // 0 switches C1 off for that button. The owner's own hand goes down to 30 ms.
    double holdMs[kButtons] = {0, 0, 0, 0, 0};          // 0 switches C2 off for that button
    bool   wheelGuard = true;
    double reverseMs = 70;      // W1: a turn this soon after the last tick is suspect
    double confirmMs = 50;      // W1: how long a suspect tick waits for one that agrees
    double firstMs = 40;        // W2: how long the first tick of a stroke waits, 0 is off
    bool   wheelFlip = true;    // a tick shown to be misread is turned round, not dropped
};

// The stamp Windows put on an event, on its coarse millisecond clock.
struct Stamp {
    bool     known = false;
    uint32_t ms = 0;
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
    bool    vetoed = false;               // the fine clock said block, the coarse clock said no
    Settled settled = Settled::None;      // an earlier held event was decided
    bool    hasEmit = false;              // send `emit` as well as letting this one through
    Emit    emit;
    bool    hasEmit2 = false;             // and `emit2` after it (two ticks waited for a third)
    Emit    emit2;
};

class Filter {
public:
    // Swaps the thresholds. What the buttons and the wheel are doing right now
    // is kept. The caller flushes first, so nothing is being held at this point.
    void configure(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }

    Outcome button(int b, bool down, double t, Stamp sys = Stamp{}) {
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
                if (coarseGapAtLeast(s.lastUpSys, sys, cfg_.bounceMs[b] + kCoarseStepMs)) {
                    o.vetoed = true;
                    s.appDown = true;
                    s.swallow = false;
                } else {
                    s.swallow = true;
                    o.block = true;
                    o.verdict = Verdict::BouncePress;
                }
            } else {
                s.appDown = true;
                s.swallow = false;
            }
            s.lastDown = t;
            s.hasDown = true;
            return o;
        }

        if (s.hasDown) o.gapMs = t - s.lastDown;
        s.lastUp = t;               // always, also for a release that is dropped or held
        s.lastUpSys = sys;
        s.hasUp = true;
        if (s.swallow) {
            s.swallow = false;
            o.block = true;
            o.verdict = Verdict::BounceRelease;
        } else if (s.pendingUp) {
            // A second release while one is already waiting. The one that is
            // waiting keeps its time; this one adds nothing.
            o.block = true;
            o.verdict = Verdict::ExtraRelease;
        } else if (!s.appDown) {
            // Release of a press the app never saw from us (tool started mid press).
        } else if (cfg_.holdMs[b] > 0) {
            s.pendingUp = true;
            s.upDeadline = t + cfg_.holdMs[b];
            o.block = true;
            o.verdict = Verdict::HeldRelease;
        } else {
            s.appDown = false;
        }
        return o;
    }

    Outcome wheel(int delta, double t, Stamp sys = Stamp{}) {
        Outcome o;
        if (delta == 0) return o;
        const int dir = delta > 0 ? 1 : -1;
        if (hasDir_) o.gapMs = t - lastAccept_;

        if (!cfg_.wheelGuard) {
            held_ = Held::None;
            accept(dir, t, sys);
            return o;
        }
        // The caller ran tick() first, so whatever is held is still inside
        // its window, and this tick settles it or joins it.
        if (held_ == Held::Reversal) {
            held_ = Held::None;
            if (dir == sign(heldDelta_)) {
                // Two ticks in a row against the stroke: the turn was real.
                o.settled = Settled::WheelConfirmed;
                send(&o, heldDelta_);
            } else {
                settleMisread(&o, heldDelta_);
            }
            accept(dir, t, sys);
            return o;
        }
        if (held_ == Held::First) {
            if (dir == sign(heldDelta_)) {
                held_ = Held::None;
                o.settled = Settled::WheelStarted;
                send(&o, heldDelta_);
                accept(dir, t, sys);
                return o;
            }
            // The first two ticks of a stroke disagree. One of them was misread
            // and nothing says which, so this one waits as well.
            held_ = Held::Pair;
            heldDelta2_ = delta;
            heldAt2_ = t;
            heldSys2_ = sys;
            o.block = true;
            o.verdict = Verdict::WheelFirst;
            return o;
        }
        if (held_ == Held::Pair) {
            // The third tick decides. The held tick that agrees with it goes
            // out as it came, the other one was the misread.
            held_ = Held::None;
            const int right = dir == sign(heldDelta_) ? heldDelta_ : heldDelta2_;
            const int wrong = dir == sign(heldDelta_) ? heldDelta2_ : heldDelta_;
            send(&o, right);
            settleMisread(&o, wrong);
            accept(dir, t, sys);
            return o;
        }

        bool inStroke = hasDir_ && (t - lastAccept_) < cfg_.reverseMs;
        if (inStroke && dir != dir_ && coarseGapAtLeast(lastAcceptSys_, sys, cfg_.reverseMs + kCoarseStepMs)) {
            // The fine clock was fooled: by the coarse clock the pause was long.
            inStroke = false;
            o.vetoed = true;
        }
        if (inStroke) {
            if (dir == dir_) {
                accept(dir, t, sys);
                return o;
            }
            hold(Held::Reversal, delta, t, sys);
            o.block = true;
            o.verdict = Verdict::WheelHeld;
            return o;
        }
        // A new stroke: there is no direction to check this tick against.
        if (cfg_.firstMs > 0) {
            hold(Held::First, delta, t, sys);
            o.block = true;
            o.verdict = Verdict::WheelFirst;
            return o;
        }
        accept(dir, t, sys);
        return o;
    }

    // Time has passed. Returns true if something was settled; `emit` is set
    // when a held release or a held first tick now has to be sent. Call again
    // until it returns false.
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
        if (held_ == Held::Reversal && t >= heldAt_ + cfg_.confirmMs) {
            held_ = Held::None;
            if (cfg_.wheelFlip) {
                // The last step of the stroke, read backwards. It did happen,
                // when the mouse sent it, and in the stroke's direction.
                emit->kind = Emit::Wheel;
                emit->delta = -heldDelta_;
                *hasEmit = true;
                *settled = Settled::WheelFlipped;
                accept(dir_, heldAt_, heldSys_);
            } else {
                // Dropped. The stroke it interrupted is over as well, so the
                // next tick starts fresh, whichever way it goes.
                hasDir_ = false;
                *settled = Settled::WheelDropped;
            }
            return true;
        }
        if (held_ == Held::First && t >= heldAt_ + cfg_.firstMs) {
            // A single notch with nothing after it: deliver it as it came.
            releaseFirst(emit);
            *settled = Settled::WheelLate;
            *hasEmit = true;
            return true;
        }
        if (held_ == Held::Pair && t >= heldAt2_ + cfg_.firstMs) {
            // Two ticks that disagree and no third one to decide. Deliver both
            // as they came: first one now, the second on the next call.
            releaseFirst(emit);
            *settled = Settled::WheelLate;
            *hasEmit = true;
            return true;
        }
        return false;
    }

    double nextDeadline() const {
        double d = std::numeric_limits<double>::infinity();
        for (int b = 0; b < kButtons; ++b)
            if (btn_[b].pendingUp && btn_[b].upDeadline < d) d = btn_[b].upDeadline;
        if (held_ == Held::Reversal && heldAt_ + cfg_.confirmMs < d) d = heldAt_ + cfg_.confirmMs;
        if (held_ == Held::First && heldAt_ + cfg_.firstMs < d) d = heldAt_ + cfg_.firstMs;
        if (held_ == Held::Pair && heldAt2_ + cfg_.firstMs < d) d = heldAt2_ + cfg_.firstMs;
        return d;
    }

    // Let go of everything that is being held back, for when the guard is
    // switched off or the program exits. Held releases are sent so no button
    // stays down for applications. Held first ticks are sent as they came. A
    // held reversed tick is most likely a misread, and is forgotten.
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
        if (held_ == Held::First || held_ == Held::Pair) {
            releaseFirst(emit);
            return true;
        }
        held_ = Held::None;
        return false;
    }

    // A release of this button is being held: hand it over for sending now.
    // For when something else is about to press that button, so the release
    // still comes first. Returns false when nothing was held.
    bool takeRelease(int b) {
        if (b < 0 || b >= kButtons || !btn_[b].pendingUp) return false;
        btn_[b].pendingUp = false;
        btn_[b].appDown = false;
        return true;
    }

    // A release of this button is being held, but applications have meanwhile
    // been given a press of it behind our back. The release is stale: forget
    // it, the button counts as down.
    void cancelRelease(int b) {
        if (b < 0 || b >= kButtons) return;
        btn_[b].pendingUp = false;
    }

    // The caller is letting through the very event the filter just decided to
    // hold. Forget that hold without sending anything: the event itself is
    // what applications get.
    void takeBackRelease(int b) {
        if (b < 0 || b >= kButtons || !btn_[b].pendingUp) return;
        btn_[b].pendingUp = false;
        btn_[b].appDown = false;
    }
    void takeBackTick() {
        if (held_ == Held::Pair) {
            // the second of a pair is the one being let through: the first stays held
            held_ = Held::First;
            accept(sign(heldDelta2_), heldAt2_, heldSys2_);
        } else if (held_ != Held::None) {
            held_ = Held::None;
            accept(sign(heldDelta_), heldAt_, heldSys_);
        }
    }

    // Stop hiding the press that is physically down right now, so its release
    // passes. For when the guard goes off or its view of the mouse may be
    // stale: from here on nothing is dropped on the strength of old state.
    void forgetHidden() {
        for (int b = 0; b < kButtons; ++b) btn_[b].swallow = false;
    }

    // True while a button press is hidden from applications, or a release or a
    // wheel tick is being held.
    bool busy() const {
        for (int b = 0; b < kButtons; ++b)
            if (btn_[b].pendingUp || btn_[b].swallow) return true;
        return held_ != Held::None;
    }

private:
    struct Btn {
        bool   appDown = false;     // what applications believe
        bool   swallow = false;     // the current physical press is hidden from them
        bool   pendingUp = false;   // a release is being held back
        double upDeadline = 0;
        bool   hasUp = false, hasDown = false;
        double lastUp = 0, lastDown = 0;
        Stamp  lastUpSys;
    };

    // True when both stamps exist and the coarse clock moved at least `ms`
    // between them. Unsigned arithmetic, so the clock wrapping round is fine.
    static bool coarseGapAtLeast(const Stamp& from, const Stamp& to, double ms) {
        if (!from.known || !to.known) return false;
        return static_cast<double>(static_cast<uint32_t>(to.ms - from.ms)) >= ms;
    }

    void accept(int dir, double t, const Stamp& sys) {
        dir_ = dir;
        lastAccept_ = t;
        lastAcceptSys_ = sys;
        hasDir_ = true;
    }

    // Reversal: one tick against the stroke. First: the first tick of a
    // stroke. Pair: the first two ticks of a stroke, which disagree.
    enum class Held : uint8_t { None, Reversal, First, Pair };

    static int sign(int delta) { return delta > 0 ? 1 : -1; }

    // Queues a held tick for sending, in the first free place.
    static void send(Outcome* o, int delta) {
        Emit& e = o->hasEmit ? o->emit2 : o->emit;
        (o->hasEmit ? o->hasEmit2 : o->hasEmit) = true;
        e.kind = Emit::Wheel;
        e.delta = delta;
    }

    // A held tick turned out to be a misread step: send it turned round, or drop it.
    void settleMisread(Outcome* o, int delta) {
        if (cfg_.wheelFlip) {
            o->settled = Settled::WheelFlipped;
            send(o, -delta);
        } else {
            o->settled = Settled::WheelDropped;
        }
    }

    void hold(Held kind, int delta, double t, const Stamp& sys) {
        held_ = kind;
        heldDelta_ = delta;
        heldAt_ = t;
        heldSys_ = sys;
    }

    // Hands the oldest held first tick over for sending. The stroke began
    // when the mouse sent it, not now, so that is the time the direction
    // counts from. If a second one is waiting behind it, that one moves up.
    void releaseFirst(Emit* emit) {
        emit->kind = Emit::Wheel;
        emit->delta = heldDelta_;
        accept(sign(heldDelta_), heldAt_, heldSys_);
        if (held_ == Held::Pair) {
            held_ = Held::First;
            heldDelta_ = heldDelta2_;
            heldAt_ = heldAt2_;
            heldSys_ = heldSys2_;
        } else {
            held_ = Held::None;
        }
    }

    Config cfg_;
    Btn    btn_[kButtons];
    bool   hasDir_ = false;     // a scroll direction is established
    int    dir_ = 0;
    double lastAccept_ = 0;
    Stamp  lastAcceptSys_;
    Held   held_ = Held::None;
    int    heldDelta_ = 0, heldDelta2_ = 0;     // the second one only while a pair waits
    double heldAt_ = 0, heldAt2_ = 0;
    Stamp  heldSys_, heldSys2_;
};

}  // namespace fbw
