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
//   W1  reversal guard    The direction of the wheel is remembered, however
//                         long ago the last tick was: a hand turns a wheel
//                         round rarely, a worn encoder misreads a step often.
//                         A tick against that direction is held back until the
//                         next tick says what it was. The same way again: the
//                         hand really turned, both are delivered, and the new
//                         direction is the one remembered from then on. The
//                         old way: the held tick was a misread step, and it is
//                         delivered turned round (wheelFlip) or dropped; but
//                         if the held tick came reverseMs or more after the
//                         stroke, a hand may have made it and the next tick
//                         may be the misread, so with wheelResend both wait
//                         for a third tick, which decides (see W2).
//                         Nothing in time: a tick that came sooner than
//                         reverseMs after the last one (no hand turns a wheel
//                         round that fast) is turned round after confirmMs; a
//                         later one is dropped after turnMs and the direction
//                         stays, though the next tick, whenever it comes, still
//                         counts as its second. So one notch back and forth is
//                         lost: the price of never moving the wrong way.
//                         turnMs 0 is the rule before: only a tick sooner than
//                         reverseMs is judged, and after a pause every stroke
//                         starts fresh.
//
//   W2  first ticks       The first tick of a stroke (nothing for reverseMs or
//                         longer) that goes the remembered way, or that has no
//                         direction to be checked against, waits up to firstMs
//                         for the second. If they agree both are delivered. If
//                         they disagree one of the two was misread, so both
//                         wait for a third tick (confirmMs with a direction
//                         remembered, else firstMs), and the odd one out is
//                         turned round or dropped. With no third tick the
//                         remembered direction decides, and with none
//                         remembered both are delivered as they came. A lone
//                         first tick is delivered as it came, firstMs late.
//                         0 switches this off.
//
// Why turning a tick round is sound, measured on the mouse this was built for
// (1270 ticks, 84 of them the wrong way): strokes with a wrong tick have the
// same number of ticks in total as strokes without one, and the gap around a
// wrong tick is two normal spacings. The wrong tick stands where a right one
// is missing. At the start of a stroke the wrong one was the first tick 11
// times and the second 4 times, which is why two ticks cannot settle it.
//
// Why the direction is remembered without a time limit, measured on the same
// mouse (one-way scroll tests, 4100 ticks): turned slowly, notch by notch, the
// wheel misread 8 to 12% of its steps, against 3 to 7% when turned fast. Every
// one of the 204 misreads was a single tick, and in slow scrolling they came at
// the normal spacing of the notches (40 to 150 ms), so no time window can tell
// them from a turn of the hand. What tells them apart is the next tick. A
// misread was the first tick after a pause often enough (seconds long) that
// the direction from before the pause is worth keeping.
//
// Sending ticks is a switch of its own (wheelResend), off by default. A tick
// this program holds back is still seen by programs that read raw input
// (measured: the hook cannot hide it from them), and a tick it sends is seen
// by them too. So whenever it sends one, those programs get it twice, or get
// the wrong one and the right one. With wheelResend off the program never
// sends a wheel tick: a misread tick is dropped, a held tick of a real turn is
// dropped as well (that notch is lost), and the first tick of a stroke is not
// held. Programs that read raw input then simply see the mouse as it is.
//
//   B   press bounce      A press shorter than blipMs is not a finger (the
//                         shortest press measured from a hand was 40 ms): the
//                         switch touched and opened again on the way down. Its
//                         release is held for blipMs, and a re-press inside
//                         that time continues the same press. Without this,
//                         C1 keeps the blip and drops the finger's real press
//                         behind it. Releases of real presses are never held.
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
    double blipMs = 20;         // B: a press shorter than this is a bounce, its release is held this long; 0 is off
    bool   wheelGuard = true;
    double reverseMs = 70;      // W1: a turn this soon after the last tick cannot be a hand
    double confirmMs = 200;     // W1: how long such a tick waits for one that agrees, then it is turned round (see keepSane)
    double turnMs = 1000;       // W1: how long a later tick against the remembered direction waits for one that agrees, then it is dropped; 0: no direction is remembered
    double firstMs = 40;        // W2: how long the first tick of a stroke waits, 0 is off (only with wheelResend)
    bool   wheelFlip = true;    // a tick shown to be misread is turned round, not dropped (only with wheelResend)
    bool   wheelResend = false; // the program may send wheel ticks itself (see above)
};

// Settings that would make the rules worse than no rules are pulled back.
// A suspect wheel tick must wait at least as long as the window that made it
// suspect: with a shorter wait, a slow real turn (notches spaced between the
// two windows) is turned round notch after notch, because each held notch
// times out before the next one confirms it and restarts the old stroke. And
// a tick that may be a hand waits at least as long as one that cannot be.
inline void keepSane(Config* c) {
    if (c->confirmMs < c->reverseMs) c->confirmMs = c->reverseMs;
    if (c->turnMs > 0 && c->turnMs < c->confirmMs) c->turnMs = c->confirmMs;
}

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
        } else if (const double hold = holdFor(b, s, t); hold > 0) {
            s.pendingUp = true;
            s.upDeadline = t + hold;
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
            if (dir == sign(heldDelta_)) {
                // Two ticks in a row against the stroke: the turn was real.
                // Its first notch goes out late, or not at all when this
                // program may not send ticks.
                held_ = Held::None;
                o.settled = Settled::WheelConfirmed;
                if (cfg_.wheelResend) send(&o, heldDelta_);
                accept(dir, t, sys);
                return o;
            }
            if (heldSlow_ && firstWait() > 0) {
                // A hand may have made the held tick (it came after a pause),
                // and then this one is the misread. One of the two is wrong,
                // and a third tick says which.
                held_ = Held::Pair;
                pairTurn_ = true;
                heldDelta2_ = delta;
                heldAt2_ = t;
                heldSys2_ = sys;
                o.block = true;
                o.verdict = Verdict::WheelFirst;
                return o;
            }
            held_ = Held::None;
            settleMisread(&o, heldDelta_);
            if (!remembers()) {
                accept(dir, t, sys);
                return o;
            }
            // The stroke went on through the misread tick. This tick is judged
            // below like any other: after a pause it is the first of a burst.
            lastAccept_ = heldAt_;
            lastAcceptSys_ = heldSys_;
        }
        else if (held_ == Held::First) {
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
            pairTurn_ = false;
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

        // A tick against the remembered direction was dropped when nothing
        // followed it in time. This tick still says what that one was: the
        // same way again, and the hand had turned; the old way, a misread.
        if (doubt_ != 0) {
            if (dir == doubt_) {
                dir_ = dir;
                lastAccept_ = doubtAt_;
                lastAcceptSys_ = doubtSys_;
            }
            doubt_ = 0;
        }

        bool inStroke = hasDir_ && (t - lastAccept_) < cfg_.reverseMs;
        if (inStroke && dir != dir_ && coarseGapAtLeast(lastAcceptSys_, sys, cfg_.reverseMs + kCoarseStepMs)) {
            // The fine clock was fooled: by the coarse clock the pause was long.
            inStroke = false;
            o.vetoed = true;
        }
        if (inStroke && dir == dir_) {
            accept(dir, t, sys);
            return o;
        }
        if (hasDir_ && dir != dir_ && (inStroke || remembers())) {
            hold(Held::Reversal, delta, t, sys);
            heldSlow_ = !inStroke;
            o.block = true;
            o.verdict = Verdict::WheelHeld;
            return o;
        }
        // A new stroke, or the remembered one going on after a pause.
        if (firstWait() > 0) {
            hold(Held::First, delta, t, sys);
            releaseAt_ = t + cfg_.firstMs;
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
        if (held_ == Held::Reversal && t >= heldAt_ + reversalWait()) {
            held_ = Held::None;
            if (!heldSlow_ && turnsTicks()) {
                // Too soon after the stroke to be a hand: the last step of the
                // stroke, read backwards. It did happen, when the mouse sent
                // it, and in the stroke's direction.
                emit->kind = Emit::Wheel;
                emit->delta = -heldDelta_;
                *hasEmit = true;
                *settled = Settled::WheelFlipped;
                accept(dir_, heldAt_, heldSys_);
            } else if (remembers()) {
                // Dropped, and the remembered direction stays. Whether the
                // hand had turned, the next tick still says (doubt_).
                doubt_ = sign(heldDelta_);
                doubtAt_ = heldAt_;
                doubtSys_ = heldSys_;
                *settled = Settled::WheelDropped;
            } else {
                // Dropped. The stroke it interrupted is over as well, so the
                // next tick starts fresh, whichever way it goes.
                hasDir_ = false;
                *settled = Settled::WheelDropped;
            }
            return true;
        }
        if (held_ == Held::First && t >= releaseAt_) {
            // A single notch with nothing after it: deliver it as it came.
            releaseFirst(emit);
            *settled = Settled::WheelLate;
            *hasEmit = true;
            return true;
        }
        if (held_ == Held::Pair && t >= heldAt2_ + pairWait()) {
            if (remembers() && hasDir_) {
                // Two ticks that disagree and no third one to decide: the one
                // against the remembered direction was misread. It goes out
                // turned round (or is dropped) now, the other one as it came
                // on the next call.
                const bool firstWrong = sign(heldDelta_) != dir_;
                const int wrong = firstWrong ? heldDelta_ : heldDelta2_;
                releaseAt_ = heldAt2_ + pairWait();     // that is now
                if (firstWrong) {
                    heldDelta_ = heldDelta2_;
                    heldAt_ = heldAt2_;
                    heldSys_ = heldSys2_;
                }
                held_ = Held::First;
                if (turnsTicks()) {
                    emit->kind = Emit::Wheel;
                    emit->delta = -wrong;
                    *hasEmit = true;
                    *settled = Settled::WheelFlipped;
                } else {
                    *settled = Settled::WheelDropped;
                }
                return true;
            }
            // No direction to go by. Deliver both as they came: first one
            // now, the second on the next call.
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
        if (held_ == Held::Reversal && heldAt_ + reversalWait() < d) d = heldAt_ + reversalWait();
        if (held_ == Held::First && releaseAt_ < d) d = releaseAt_;
        if (held_ == Held::Pair && heldAt2_ + pairWait() < d) d = heldAt2_ + pairWait();
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
        doubt_ = 0;
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
            // The second of a pair is the one being let through: the first
            // stays held, as what it was before the second came.
            held_ = pairTurn_ ? Held::Reversal : Held::First;
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

    // The wheel direction the rules go by: 1 up, -1 down, 0 none yet.
    int remembered() const { return hasDir_ ? dir_ : 0; }

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

    bool   turnsTicks() const { return cfg_.wheelResend && cfg_.wheelFlip; }
    double firstWait() const { return cfg_.wheelResend ? cfg_.firstMs : 0; }
    bool   remembers() const { return cfg_.turnMs > 0; }
    // A tick too soon after the stroke to be a hand waits confirmMs, a later
    // one turnMs.
    double reversalWait() const { return heldSlow_ ? cfg_.turnMs : cfg_.confirmMs; }
    // Two held ticks that disagree wait this long for a third. With a direction
    // remembered, as long as a suspect tick waits for one that agrees: in slow
    // scrolling the third tick is often more than firstMs away.
    double pairWait() const { return remembers() ? cfg_.confirmMs : cfg_.firstMs; }

    // How long to hold a release: the release delay of the button (C2), or,
    // for a press too short to be a finger, the press bounce window (B).
    double holdFor(int b, const Btn& s, double t) const {
        const bool blip = cfg_.blipMs > 0 && s.hasDown && (t - s.lastDown) < cfg_.blipMs;
        const double c2 = cfg_.holdMs[b];
        return blip && cfg_.blipMs > c2 ? cfg_.blipMs : c2;
    }

    // Queues a held tick for sending, in the first free place.
    static void send(Outcome* o, int delta) {
        Emit& e = o->hasEmit ? o->emit2 : o->emit;
        (o->hasEmit ? o->hasEmit2 : o->hasEmit) = true;
        e.kind = Emit::Wheel;
        e.delta = delta;
    }

    // A held tick turned out to be a misread step: send it turned round, or drop it.
    void settleMisread(Outcome* o, int delta) {
        if (turnsTicks()) {
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
            releaseAt_ = heldAt2_ + pairWait();     // due when the pair was: now, unless flushed
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
    double releaseAt_ = 0;      // when a held first tick goes out
    Stamp  heldSys_, heldSys2_;
    bool   heldSlow_ = false;   // the held reversal came reverseMs or more after the stroke: a hand may have done it
    bool   pairTurn_ = false;   // the pair began as a held reversal
    int    doubt_ = 0;          // direction of a reversal dropped for want of a second tick, 0 none
    double doubtAt_ = 0;
    Stamp  doubtSys_;
};

}  // namespace fbw
