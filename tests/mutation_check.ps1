<#
Proves the rule tests can fail. Each entry below breaks one line of filter.h in
a throwaway copy; the tests must then fail. A broken copy that still passes
means a rule has no test guarding it.

  mutation_check.ps1        exit code 0 when every broken copy is caught
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'src'
$work = Join-Path $root 'build\mutants'
$nl = "`r`n"

$mutants = @(
    # C1: re-press guard
    @{ name = 'C1 threshold is inclusive';            find = '(t - s.lastUp) < cfg_.bounceMs[b]';   put = '(t - s.lastUp) <= cfg_.bounceMs[b]' },
    @{ name = 'C1 forgets to drop the release';       find = "                    s.swallow = true;$nl                    o.block = true;$nl                    o.verdict = Verdict::BouncePress;"; put = "                    o.block = true;$nl                    o.verdict = Verdict::BouncePress;" },
    @{ name = 'C1 passes the bounce press';           find = "                    s.swallow = true;$nl                    o.block = true;"; put = "                    s.swallow = true;$nl                    o.block = false;" },
    @{ name = 'C1 ignores dropped releases for the gap'; find = '        s.lastUp = t;               // always'; put = '        if (!s.swallow) s.lastUp = t;   // always' },
    @{ name = 'C1 applies to the first press ever';   find = 'cfg_.clickGuard && s.hasUp && (t - s.lastUp)'; put = 'cfg_.clickGuard && (t - s.lastUp)' },
    @{ name = 'C1 ignores the on/off switch';         find = 'cfg_.clickGuard && s.hasUp && (t - s.lastUp)'; put = 's.hasUp && (t - s.lastUp)' },
    @{ name = 'forgetHidden does nothing';            find = 'for (int b = 0; b < kButtons; ++b) btn_[b].swallow = false;'; put = 'for (int b = 0; b < 0; ++b) btn_[b].swallow = false;' },

    # C2: hold bridge
    @{ name = 'C2 doubles the hold';                  find = 's.upDeadline = t + hold;';            put = 's.upDeadline = t + hold * 2;' },
    @{ name = 'C2 hold is not per button';            find = 'const double c2 = cfg_.holdMs[b];';   put = 'const double c2 = cfg_.holdMs[Left];' },
    @{ name = 'B no press bounce bridge';             find = 'const bool blip = cfg_.blipMs > 0 && s.hasDown && (t - s.lastDown) < cfg_.blipMs;'; put = 'const bool blip = false;' },
    @{ name = 'B a 20 ms press counts as a bounce';   find = 'const bool blip = cfg_.blipMs > 0 && s.hasDown && (t - s.lastDown) < cfg_.blipMs;'; put = 'const bool blip = cfg_.blipMs > 0 && s.hasDown && (t - s.lastDown) <= cfg_.blipMs;' },
    @{ name = 'B every release is held';              find = 'const bool blip = cfg_.blipMs > 0 && s.hasDown && (t - s.lastDown) < cfg_.blipMs;'; put = 'const bool blip = cfg_.blipMs > 0;' },
    @{ name = 'B the bounce window is ignored when shorter than the hold'; find = 'return blip && cfg_.blipMs > c2 ? cfg_.blipMs : c2;'; put = 'return blip ? cfg_.blipMs : c2;' },
    @{ name = 'keepSane does nothing';                find = 'if (c->confirmMs < c->reverseMs) c->confirmMs = c->reverseMs;'; put = '(void)c;' },
    @{ name = 'C2 re-press does not cancel the release'; find = "                s.pendingUp = false;$nl                o.block = true;$nl                o.verdict = Verdict::BridgedPress;"; put = "                o.block = true;$nl                o.verdict = Verdict::BridgedPress;" },
    @{ name = 'C2 passes the bridging press';         find = "                o.block = true;$nl                o.verdict = Verdict::BridgedPress;"; put = "                o.block = false;$nl                o.verdict = Verdict::BridgedPress;" },
    @{ name = 'C2 holds releases the app never saw pressed'; find = '        } else if (!s.appDown) {'; put = '        } else if (false) {' },
    @{ name = 'C2 release deadline is exclusive';     find = 's.pendingUp && t >= s.upDeadline';    put = 's.pendingUp && t > s.upDeadline' },
    @{ name = 'C2 second release pushes the held one back'; find = "            o.block = true;$nl            o.verdict = Verdict::ExtraRelease;"; put = "            s.upDeadline = t + cfg_.holdMs[b];$nl            o.block = true;$nl            o.verdict = Verdict::ExtraRelease;" },
    @{ name = 'C2 second release gets through';       find = "            o.block = true;$nl            o.verdict = Verdict::ExtraRelease;"; put = "            o.block = false;$nl            o.verdict = Verdict::ExtraRelease;" },
    @{ name = 'C2 flush forgets the button';          find = "                emit->kind = Emit::ButtonUp;$nl                emit->button = b;$nl                return true;$nl            }$nl        }$nl        if (held_ == Held::Pair && remembers()) {"; put = "                emit->kind = Emit::ButtonUp;$nl                emit->button = 0;$nl                return false;$nl            }$nl        }$nl        if (held_ == Held::Pair && remembers()) {" },

    # W1: a tick against the stroke
    @{ name = 'W1 window is inclusive';               find = '(t - lastAccept_) < cfg_.reverseMs';  put = '(t - lastAccept_) <= cfg_.reverseMs' },
    @{ name = 'W1 judges the very first tick against no direction'; find = 'bool inStroke = hasDir_ && (t - lastAccept_) < cfg_.reverseMs;'; put = 'bool inStroke = (t - lastAccept_) < cfg_.reverseMs;' },
    @{ name = 'W1 passes the held tick';              find = "            o.block = true;$nl            o.verdict = Verdict::WheelHeld;"; put = "            o.block = false;$nl            o.verdict = Verdict::WheelHeld;" },
    @{ name = 'W1 ignores the on/off switch';         find = 'if (!cfg_.wheelGuard) {';             put = 'if (false) {' },
    @{ name = 'W1 a real turn: the held tick is not sent'; find = "                    send(&o, heldDelta_);$nl                } else {$nl                    o.settled = Settled::WheelTurnLost;"; put = "                } else {$nl                    o.settled = Settled::WheelTurnLost;" },
    @{ name = 'W1 a real turn: the held tick is sent with the wrong size'; find = "                    send(&o, heldDelta_);$nl                } else {"; put = "                    send(&o, delta);$nl                } else {" },
    @{ name = 'without sending, a real turn still sends';  find = "                    settledDelta_ = heldDelta_;$nl                }$nl                accept(dir, t, sys);"; put = "                    settledDelta_ = heldDelta_;$nl                    send(&o, heldDelta_);$nl                }$nl                accept(dir, t, sys);" },
    @{ name = 'a lost first notch does not say which way it went'; find = "                    o.settled = Settled::WheelTurnLost;$nl                    settledDelta_ = heldDelta_;"; put = "                    o.settled = Settled::WheelTurnLost;" },
    @{ name = 'without sending, a lost first notch is reported as sent'; find = 'o.settled = Settled::WheelTurnLost;'; put = 'o.settled = Settled::WheelConfirmed;' },
    @{ name = 'without sending, misreads are still turned'; find = 'bool   turnsTicks() const { return cfg_.wheelResend && cfg_.wheelFlip; }'; put = 'bool   turnsTicks() const { return cfg_.wheelFlip; }' },
    @{ name = 'without sending, the first tick still waits'; find = 'double firstWait() const { return cfg_.wheelResend ? cfg_.firstMs : 0; }'; put = 'double firstWait() const { return cfg_.firstMs; }' },
    @{ name = 'W1 a confirmed turn leaves its tick held'; find = "                held_ = Held::None;$nl                if (cfg_.wheelResend) {"; put = "                if (cfg_.wheelResend) {" },
    @{ name = 'W1 a misread leaves its tick held';    find = "            held_ = Held::None;$nl            settleMisread(&o, heldDelta_);"; put = "            settleMisread(&o, heldDelta_);" },
    @{ name = 'W1 a confirmed turn does not take up the new direction'; find = "                    settledDelta_ = heldDelta_;$nl                }$nl                accept(dir, t, sys);$nl                return o;"; put = "                    settledDelta_ = heldDelta_;$nl                }$nl                return o;" },
    @{ name = 'W1 the rule before does not take up the direction after a misread'; find = "            if (!remembers()) {$nl                accept(dir, t, sys);$nl                return o;$nl            }"; put = "            if (!remembers()) {$nl                return o;$nl            }" },
    @{ name = 'W1 confirm window is exclusive';       find = 'if (held_ == Held::Reversal && t >= heldAt_ + reversalWait()) {'; put = 'if (held_ == Held::Reversal && t > heldAt_ + reversalWait()) {' },
    @{ name = 'W1 never settles a held tick by time'; find = 'if (held_ == Held::Reversal && t >= heldAt_ + reversalWait()) {'; put = 'if (false) {' },
    @{ name = 'W1 the wake-up time of a held tick is wrong'; find = 'if (held_ == Held::Reversal && heldAt_ + reversalWait() < d) d = heldAt_ + reversalWait();'; put = 'if (held_ == Held::Reversal && heldAt_ + cfg_.confirmMs < d) d = heldAt_ + cfg_.confirmMs;' },
    @{ name = 'W1 keeps the direction after a dropped tick'; find = "                hasDir_ = false;$nl                *settled = Settled::WheelDropped;"; put = "                *settled = Settled::WheelDropped;" },

    # the remembered direction
    @{ name = 'a later tick against the direction is not held';  find = 'if (hasDir_ && dir != dir_ && (inStroke || remembers())) {'; put = 'if (hasDir_ && dir != dir_ && inStroke) {' },
    @{ name = 'the direction is remembered with turnMs 0';      find = 'bool   remembers() const { return cfg_.turnMs > 0; }'; put = 'bool   remembers() const { return true; }' },
    @{ name = 'a slow tick against it waits only confirmMs';    find = 'double reversalWait() const { return heldSlow_ ? cfg_.turnMs : cfg_.confirmMs; }'; put = 'double reversalWait() const { return cfg_.confirmMs; }' },
    @{ name = 'a quick tick against it waits turnMs';           find = 'double reversalWait() const { return heldSlow_ ? cfg_.turnMs : cfg_.confirmMs; }'; put = 'double reversalWait() const { return cfg_.turnMs; }' },
    @{ name = 'a slow lone tick is turned round, not dropped';  find = 'if (!heldSlow_ && turnsTicks()) {'; put = 'if (turnsTicks()) {' },
    @{ name = 'a quick lone tick is dropped although turning is on'; find = 'if (!heldSlow_ && turnsTicks()) {'; put = 'if (false) {' },
    @{ name = 'a held tick is never taken as one a hand may have made'; find = 'heldSlow_ = !inStroke;'; put = 'heldSlow_ = false;' },
    @{ name = 'a quick misread that may not be turned round lends the next tick a direction'; find = '            } else if (!heldSlow_ && remembers()) {'; put = '            } else if (false) {' },
    @{ name = 'a quick misread dropped does not let the stroke go on'; find = "                *settled = Settled::WheelDropped;$nl                accept(dir_, heldAt_, heldSys_);"; put = "                *settled = Settled::WheelDropped;" },
    @{ name = 'with nothing remembered, a pair with no third tick goes out as it came'; find = "            if (remembers()) {$nl                // Nothing remembered yet"; put = "            if (false) {$nl                // Nothing remembered yet" },
    @{ name = 'what was settled: the time-out of a held tick does not say which way it went'; find = "            held_ = Held::None;$nl            settledDelta_ = heldDelta_;"; put = "            held_ = Held::None;$nl            settledDelta_ = 0;" },
    @{ name = 'what was settled: a misread is reported the wrong way round'; find = '        settledDelta_ = delta;'; put = '        settledDelta_ = -delta;' },
    @{ name = 'what was settled: a pair reports the wrong tick'; find = '                settledDelta_ = wrong;'; put = '                settledDelta_ = -wrong;' },
    @{ name = 'a pair counts as one held tick';      find = 'return held_ == Held::Pair ? 2 : held_ == Held::None ? 0 : 1;'; put = 'return held_ == Held::None ? 0 : 1;' },
    @{ name = 'a dropped tick does not count toward a turn';    find = '                doubt_ = sign(heldDelta_);'; put = '                doubt_ = 0;' },
    @{ name = 'a dropped tick and the next one the same way are not a turn'; find = "            if (dir == doubt_) {$nl                dir_ = dir;"; put = "            if (false) {$nl                dir_ = dir;" },
    @{ name = 'a turn after a dropped tick counts from now';    find = '                lastAccept_ = doubtAt_;'; put = '                lastAccept_ = t;' },
    @{ name = 'a dropped tick counts toward a turn forever';    find = "            doubt_ = 0;$nl        }$nl$nl        bool inStroke"; put = "        }$nl$nl        bool inStroke" },
    @{ name = 'flush keeps a dropped tick in doubt';            find = "        held_ = Held::None;$nl        doubt_ = 0;$nl        return false;"; put = "        held_ = Held::None;$nl        return false;" },
    @{ name = 'a held slow tick and the next one against it do not wait for a third'; find = 'if (heldSlow_ && cfg_.wheelResend) {'; put = 'if (false) {' },
    @{ name = 'a held quick tick and the next one against it wait for a third'; find = 'if (heldSlow_ && cfg_.wheelResend) {'; put = 'if (cfg_.wheelResend) {' },
    @{ name = 'the third tick vote is off when the first tick wait is'; find = 'if (heldSlow_ && cfg_.wheelResend) {'; put = 'if (heldSlow_ && firstWait() > 0) {' },
    @{ name = 'a quick misread: the next tick is taken as in the stroke'; find = "            lastAccept_ = heldAt_;$nl            lastAcceptSys_ = heldSys_;$nl        }"; put = "            accept(dir, t, sys);$nl            return o;$nl        }" },
    @{ name = 'a pair waits only firstMs with a direction remembered'; find = 'double pairWait() const { return remembers() ? cfg_.confirmMs : cfg_.firstMs; }'; put = 'double pairWait() const { return cfg_.firstMs; }' },
    @{ name = 'a pair with no third tick ignores the remembered direction'; find = 'if (remembers() && hasDir_) {'; put = 'if (false) {' },
    @{ name = 'a pair with no third tick blames the wrong one';  find = 'const bool firstWrong = sign(heldDelta_) != dir_;'; put = 'const bool firstWrong = sign(heldDelta_) == dir_;' },
    @{ name = 'a pair with no third tick sends its second one late'; find = 'releaseAt_ = heldAt2_ + pairWait();     // that is now'; put = 'releaseAt_ = heldAt2_ + 2 * pairWait();     // that is now' },
    @{ name = 'keepSane lets the turn wait under the confirm window'; find = 'if (c->turnMs > 0 && c->turnMs < c->confirmMs) c->turnMs = c->confirmMs;'; put = '(void)0;' },
    @{ name = 'keepSane switches the remembered direction on'; find = 'if (c->turnMs > 0 && c->turnMs < c->confirmMs) c->turnMs = c->confirmMs;'; put = 'if (c->turnMs < c->confirmMs) c->turnMs = c->confirmMs;' },

    # turning a misread tick round
    @{ name = 'a misread tick is dropped although turning is on'; find = "        if (turnsTicks()) {$nl            o->settled = Settled::WheelFlipped;"; put = "        if (false) {$nl            o->settled = Settled::WheelFlipped;" },
    @{ name = 'a misread tick is turned although turning is off'; find = "        if (turnsTicks()) {$nl            o->settled = Settled::WheelFlipped;"; put = "        if (true) {$nl            o->settled = Settled::WheelFlipped;" },
    @{ name = 'a turned tick goes out the wrong way round'; find = 'send(o, -delta);';                put = 'send(o, delta);' },
    @{ name = 'a turned last tick goes out the wrong way round'; find = 'emit->delta = -heldDelta_;'; put = 'emit->delta = heldDelta_;' },
    @{ name = 'a turned last tick does not count as part of the stroke'; find = "                *settled = Settled::WheelFlipped;$nl                accept(dir_, heldAt_, heldSys_);"; put = "                *settled = Settled::WheelFlipped;" },

    # W2: the first ticks of a stroke
    @{ name = 'W2 the first tick is not held';        find = 'if (firstWait() > 0) {';              put = 'if (false) {' },
    @{ name = 'W2 a lone first tick is never sent';   find = 'if (held_ == Held::First && t >= releaseAt_) {'; put = 'if (false) {' },
    @{ name = 'W2 first tick window is exclusive';    find = 'if (held_ == Held::First && t >= releaseAt_) {'; put = 'if (held_ == Held::First && t > releaseAt_) {' },
    @{ name = 'W2 a first tick waits twice as long';  find = 'releaseAt_ = t + (firstEver ? cfg_.confirmMs : cfg_.firstMs);'; put = 'releaseAt_ = t + (firstEver ? cfg_.confirmMs : 2 * cfg_.firstMs);' },
    @{ name = 'the first tick since start waits only firstMs'; find = 'const bool firstEver = remembers() && !hasDir_ && cfg_.confirmMs > cfg_.firstMs;'; put = 'const bool firstEver = false;' },
    @{ name = 'the first tick since start waits confirmMs even when the first tick wait is longer'; find = 'const bool firstEver = remembers() && !hasDir_ && cfg_.confirmMs > cfg_.firstMs;'; put = 'const bool firstEver = remembers() && !hasDir_;' },
    @{ name = 'the start pair drop names a tick';    find = "                settledDelta_ = 0;$nl                *settled = Settled::WheelDropped;"; put = "                settledDelta_ = heldDelta_;$nl                *settled = Settled::WheelDropped;" },
    @{ name = 'a stall at start lets the first tick out after the second'; find = '            if (nothingRemembered) held_ = Held::None;'; put = '' },
    @{ name = 'every first tick waits confirmMs';      find = 'const bool firstEver = remembers() && !hasDir_ && cfg_.confirmMs > cfg_.firstMs;'; put = 'const bool firstEver = remembers() && cfg_.confirmMs > cfg_.firstMs;' },
    @{ name = 'W2 the wake-up time of a first tick is wrong'; find = 'if (held_ == Held::First && releaseAt_ < d) d = releaseAt_;'; put = 'if (held_ == Held::First && heldAt_ + cfg_.firstMs < d) d = heldAt_ + cfg_.firstMs;' },
    @{ name = 'W2 a pair let out by time sends its second tick late'; find = 'releaseAt_ = heldAt2_ + pairWait();     // due when the pair was: now, unless flushed'; put = 'releaseAt_ = heldAt2_ + 2 * pairWait();     // due when the pair was: now, unless flushed' },
    @{ name = 'W2 the stroke counts from the late delivery'; find = "        emit->delta = heldDelta_;$nl        accept(sign(heldDelta_), heldAt_, heldSys_);"; put = "        emit->delta = heldDelta_;$nl        accept(sign(heldDelta_), heldAt_ + cfg_.firstMs, heldSys_);" },
    @{ name = 'W2 an agreeing second tick does not release the first'; find = "                held_ = Held::None;$nl                o.settled = Settled::WheelStarted;$nl                send(&o, heldDelta_);$nl                accept(dir, t, sys);"; put = "                held_ = Held::None;$nl                o.settled = Settled::WheelStarted;$nl                accept(dir, t, sys);" },
    @{ name = 'W2 the second of a disagreeing pair is let through'; find = "            o.block = true;$nl            o.verdict = Verdict::WheelFirst;$nl            return o;$nl        }$nl        if (held_ == Held::Pair) {"; put = "            o.block = false;$nl            o.verdict = Verdict::WheelFirst;$nl            return o;$nl        }$nl        if (held_ == Held::Pair) {" },
    @{ name = 'W2 the third tick blames the wrong one of the pair'; find = 'const int right = dir == sign(heldDelta_) ? heldDelta_ : heldDelta2_;'; put = 'const int right = dir == sign(heldDelta_) ? heldDelta2_ : heldDelta_;' },
    @{ name = 'W2 the third tick sends only one of the pair'; find = "            send(&o, right);$nl            settleMisread(&o, wrong);"; put = "            settleMisread(&o, wrong);" },
    @{ name = 'W2 a pair with no third tick loses its second tick'; find = "        if (held_ == Held::Pair) {$nl            held_ = Held::First;"; put = "        if (false) {$nl            held_ = Held::First;" },
    @{ name = 'W2 a pair is timed from its first tick'; find = 'if (held_ == Held::Pair && t >= heldAt2_ + pairWait()) {'; put = 'if (held_ == Held::Pair && t >= heldAt_ + pairWait()) {' },
    @{ name = 'W2 a pair is never settled by time';   find = 'if (held_ == Held::Pair && t >= heldAt2_ + pairWait()) {'; put = 'if (false) {' },
    @{ name = 'W2 the wake-up time of a pair is wrong'; find = 'if (held_ == Held::Pair && heldAt2_ + pairWait() < d) d = heldAt2_ + pairWait();'; put = 'if (held_ == Held::Pair && heldAt_ + pairWait() < d) d = heldAt_ + pairWait();' },
    @{ name = 'flush forgets held first ticks';       find = "        if (held_ == Held::First || held_ == Held::Pair) {$nl            releaseFirst(emit);$nl            return true;"; put = "        if (false) {$nl            releaseFirst(emit);$nl            return true;" },

    # coarse clock veto
    @{ name = 'veto is ignored for clicks';           find = 'if (coarseGapAtLeast(s.lastUpSys, sys, cfg_.bounceMs[b] + kCoarseStepMs)) {'; put = 'if (false) {' },
    @{ name = 'veto for clicks has no clock step margin'; find = 'if (coarseGapAtLeast(s.lastUpSys, sys, cfg_.bounceMs[b] + kCoarseStepMs)) {'; put = 'if (coarseGapAtLeast(s.lastUpSys, sys, cfg_.bounceMs[b])) {' },
    @{ name = 'a vetoed press is not counted as down'; find = "                    o.vetoed = true;$nl                    s.appDown = true;"; put = "                    o.vetoed = true;" },
    @{ name = 'veto is ignored for the wheel';        find = 'if (inStroke && dir != dir_ && coarseGapAtLeast(lastAcceptSys_, sys, cfg_.reverseMs + kCoarseStepMs)) {'; put = 'if (false) {' },
    @{ name = 'veto for the wheel has no clock step margin'; find = 'if (inStroke && dir != dir_ && coarseGapAtLeast(lastAcceptSys_, sys, cfg_.reverseMs + kCoarseStepMs)) {'; put = 'if (inStroke && dir != dir_ && coarseGapAtLeast(lastAcceptSys_, sys, cfg_.reverseMs)) {' },
    @{ name = 'veto delays a tick that goes with the stroke'; find = 'if (inStroke && dir != dir_ && coarseGapAtLeast(lastAcceptSys_, sys, cfg_.reverseMs + kCoarseStepMs)) {'; put = 'if (inStroke && coarseGapAtLeast(lastAcceptSys_, sys, cfg_.reverseMs + kCoarseStepMs)) {' },
    @{ name = 'veto works with a stamp on one side only'; find = 'if (!from.known || !to.known) return false;'; put = 'if (!to.known) return false;' },
    @{ name = 'veto breaks when the coarse clock wraps'; find = 'static_cast<double>(static_cast<uint32_t>(to.ms - from.ms)) >= ms'; put = '(static_cast<double>(to.ms) - static_cast<double>(from.ms)) >= ms' },

    # housekeeping
    @{ name = 'busy() forgets hidden presses';        find = 'if (btn_[b].pendingUp || btn_[b].swallow) return true;'; put = 'if (btn_[b].pendingUp) return true;' },

    # taking a hold back
    @{ name = 'takeRelease leaves the release held';  find = "        btn_[b].pendingUp = false;$nl        btn_[b].appDown = false;$nl        return true;"; put = "        btn_[b].appDown = false;$nl        return true;" },
    @{ name = 'takeRelease leaves the button down';   find = "        btn_[b].pendingUp = false;$nl        btn_[b].appDown = false;$nl        return true;"; put = "        btn_[b].pendingUp = false;$nl        return true;" },
    @{ name = 'cancelRelease also marks the button up'; find = "        if (b < 0 || b >= kButtons) return;$nl        btn_[b].pendingUp = false;"; put = "        if (b < 0 || b >= kButtons) return;$nl        btn_[b].pendingUp = false;$nl        btn_[b].appDown = false;" },
    @{ name = 'takeBackRelease leaves the button down'; find = "        btn_[b].pendingUp = false;$nl        btn_[b].appDown = false;$nl    }"; put = "        btn_[b].pendingUp = false;$nl    }" },
    @{ name = 'takeBackTick keeps the tick held';     find = "        } else if (held_ != Held::None) {$nl            held_ = Held::None;"; put = "        } else if (held_ != Held::None) {" },
    @{ name = 'takeBackTick takes back the whole pair'; find = "            held_ = pairTurn_ ? Held::Reversal : Held::First;$nl            accept(sign(heldDelta2_), heldAt2_, heldSys2_);"; put = "            held_ = Held::None;$nl            accept(sign(heldDelta2_), heldAt2_, heldSys2_);" },
    @{ name = 'letting go sends a pair as it came';    find = 'if (held_ == Held::Pair && remembers()) {'; put = 'if (false) {' },
    @{ name = 'letting go at start sends a pair as it came'; find = "            else held_ = Held::None;"; put = "            else {}" },
    @{ name = 'takeBackTick makes a held turn a first tick'; find = '            held_ = pairTurn_ ? Held::Reversal : Held::First;'; put = '            held_ = Held::First;' },
    @{ name = 'takeBackTick does not let the stroke go on'; find = "            held_ = Held::None;$nl            accept(sign(heldDelta_), heldAt_, heldSys_);$nl        }$nl    }"; put = "            held_ = Held::None;$nl        }$nl    }" },

    # which mouse (whichmouse.h)
    @{ file = 'whichmouse.h'; name = 'a raw record with no device is kept';          find = '        if (!device) return;'; put = '' },
    @{ file = 'whichmouse.h'; name = 'the newest matching record is used first';     find = 'if (best < 0 || s.seq < slots_[best].seq) best = i;'; put = 'if (best < 0 || s.seq > slots_[best].seq) best = i;' },
    @{ file = 'whichmouse.h'; name = 'ties go by time, not by arrival';              find = 'if (best < 0 || s.seq < slots_[best].seq) best = i;'; put = 'if (best < 0 || s.t < slots_[best].t) best = i;' },
    @{ file = 'whichmouse.h'; name = 'a record stays fresh far too long';            find = 'constexpr double kRawFreshMs = 200;'; put = 'constexpr double kRawFreshMs = 2000;' },
    @{ file = 'whichmouse.h'; name = 'a record is stale almost at once';             find = 'constexpr double kRawFreshMs = 200;'; put = 'constexpr double kRawFreshMs = 2;' },
    @{ file = 'whichmouse.h'; name = 'the last active mouse is remembered for hours'; find = 'constexpr double kLastMouseMs = 500;'; put = 'constexpr double kLastMouseMs = 1e9;' },
    @{ file = 'whichmouse.h'; name = 'movement takes a slot';                         find = "        lastAt_ = t;$nl        if (!flags) return;"; put = "        lastAt_ = t;" },
    @{ file = 'whichmouse.h'; name = 'only two records are kept';                     find = 'static constexpr int kKeep = 32;'; put = 'static constexpr int kKeep = 2;' },
    @{ file = 'pairs.h'; name = 'pairs are looked at only up to 26 ms';               find = 'constexpr double kSuspectGapMs = 60;'; put = 'constexpr double kSuspectGapMs = 26;' },
    @{ file = 'pairs.h'; name = 'a 10 to 20 ms gap counts as a finger';               find = 'constexpr double kImpossibleGapMs = 20;'; put = 'constexpr double kImpossibleGapMs = 10;' },
    @{ file = 'pairs.h'; name = 'a 10 to 20 ms press counts as a finger';             find = 'constexpr double kBlipMs = 20;'; put = 'constexpr double kBlipMs = 10;' },
    @{ file = 'pairs.h'; name = 'a blip as the first press is not looked at';         find = '(holdBeforeMs >= 0 && holdBeforeMs < kBlipMs)'; put = '(false)' },
    @{ file = 'pairs.h'; name = 'a blip as the second press is not looked at';        find = '(holdAfterMs >= 0 && holdAfterMs < kBlipMs)'; put = '(false)' },
    @{ file = 'whichmouse.h'; name = 'a used record can be used again';              find = 'slots_[best].flags = static_cast<uint16_t>(slots_[best].flags & ~flag);'; put = '' },
    @{ file = 'whichmouse.h'; name = 'a stale record is still matched';              find = 'if (!(s.flags & flag) || now - s.t > kRawFreshMs) continue;'; put = 'if (!(s.flags & flag)) continue;' },
    @{ file = 'whichmouse.h'; name = 'the last active mouse never expires';          find = 'if (last_ && now - lastAt_ <= kLastMouseMs) {'; put = 'if (last_) {' },
    @{ file = 'whichmouse.h'; name = 'movement does not make a mouse the last active one'; find = "        last_ = device;$nl        lastAt_ = t;$nl        if (!flags) return;"; put = "        if (!flags) return;$nl        last_ = device;$nl        lastAt_ = t;" },
    @{ file = 'whichmouse.h'; name = 'a match by last activity is reported as exact'; find = '            *how = kDevLastActive;'; put = '            *how = kDevExact;' }
    # Not here on purpose: breaking the range check on the button number. The
    # broken copy then reads and writes outside the array, and what happens
    # next depends on what the compiler put there, so a test cannot promise to
    # notice it. The hook only ever passes numbers 0 to 4.
)

# Each entry breaks filter.h unless it names another file. The broken copy sits
# alone in its folder, which is searched before the real sources.
$originals = @{}
foreach ($name in 'filter.h', 'whichmouse.h', 'pairs.h') {
    $text = [IO.File]::ReadAllText((Join-Path $src $name))
    if ($text -notmatch "`r`n") { $text = $text -replace "`n", "`r`n" }
    $originals[$name] = $text
}

$survived = @()
$i = 0
foreach ($m in $mutants) {
    $i++
    $file = if ($m.file) { $m.file } else { 'filter.h' }
    $original = $originals[$file]
    $count = ([regex]::Matches($original, [regex]::Escape($m.find))).Count
    if ($count -ne 1) { throw "mutant '$($m.name)': the text to break occurs $count times in $file, expected exactly 1" }
    $dir = Join-Path $work "m$i"
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir $file), $original.Replace($m.find, $m.put))
    $caught = $false
    try {
        & (Join-Path $root 'build.ps1') -Only tests -SrcDir $dir -OutDir (Join-Path $dir 'out') *> (Join-Path $dir 'log.txt')
    } catch {
        $caught = $true
    }
    if ($caught) { Write-Host ("caught    {0}" -f $m.name) }
    else { Write-Host ("SURVIVED  {0}" -f $m.name); $survived += $m.name }
}

Write-Host ("{0} mutants, {1} caught, {2} survived" -f $mutants.Count, ($mutants.Count - $survived.Count), $survived.Count)
if ($survived.Count) { exit 1 }
exit 0
