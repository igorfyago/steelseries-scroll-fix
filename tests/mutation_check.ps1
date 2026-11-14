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
    @{ name = 'C2 doubles the hold';                  find = 's.upDeadline = t + cfg_.holdMs[b];';  put = 's.upDeadline = t + cfg_.holdMs[b] * 2;' },
    @{ name = 'C2 hold is not per button';            find = '} else if (cfg_.holdMs[b] > 0) {';    put = '} else if (cfg_.holdMs[Left] > 0) {' },
    @{ name = 'C2 re-press does not cancel the release'; find = "                s.pendingUp = false;$nl                o.block = true;$nl                o.verdict = Verdict::BridgedPress;"; put = "                o.block = true;$nl                o.verdict = Verdict::BridgedPress;" },
    @{ name = 'C2 passes the bridging press';         find = "                o.block = true;$nl                o.verdict = Verdict::BridgedPress;"; put = "                o.block = false;$nl                o.verdict = Verdict::BridgedPress;" },
    @{ name = 'C2 holds releases the app never saw pressed'; find = '        } else if (!s.appDown) {'; put = '        } else if (false) {' },
    @{ name = 'C2 release deadline is exclusive';     find = 's.pendingUp && t >= s.upDeadline';    put = 's.pendingUp && t > s.upDeadline' },
    @{ name = 'C2 second release pushes the held one back'; find = "            o.block = true;$nl            o.verdict = Verdict::ExtraRelease;"; put = "            s.upDeadline = t + cfg_.holdMs[b];$nl            o.block = true;$nl            o.verdict = Verdict::ExtraRelease;" },
    @{ name = 'C2 second release gets through';       find = "            o.block = true;$nl            o.verdict = Verdict::ExtraRelease;"; put = "            o.block = false;$nl            o.verdict = Verdict::ExtraRelease;" },
    @{ name = 'C2 flush forgets the button';          find = "                emit->kind = Emit::ButtonUp;$nl                emit->button = b;$nl                return true;$nl            }$nl        }$nl        if (held_ == Held::First"; put = "                emit->kind = Emit::ButtonUp;$nl                emit->button = 0;$nl                return false;$nl            }$nl        }$nl        if (held_ == Held::First" },

    # W1: a tick against the stroke
    @{ name = 'W1 window is inclusive';               find = '(t - lastAccept_) < cfg_.reverseMs';  put = '(t - lastAccept_) <= cfg_.reverseMs' },
    @{ name = 'W1 judges the very first tick against no direction'; find = 'bool inStroke = hasDir_ && (t - lastAccept_) < cfg_.reverseMs;'; put = 'bool inStroke = (t - lastAccept_) < cfg_.reverseMs;' },
    @{ name = 'W1 passes the held tick';              find = "            o.block = true;$nl            o.verdict = Verdict::WheelHeld;"; put = "            o.block = false;$nl            o.verdict = Verdict::WheelHeld;" },
    @{ name = 'W1 ignores the on/off switch';         find = 'if (!cfg_.wheelGuard) {';             put = 'if (false) {' },
    @{ name = 'W1 a real turn: the held tick is not sent'; find = "                o.settled = Settled::WheelConfirmed;$nl                send(&o, heldDelta_);$nl            } else {$nl                settleMisread(&o, heldDelta_);"; put = "                o.settled = Settled::WheelConfirmed;$nl            } else {$nl                settleMisread(&o, heldDelta_);" },
    @{ name = 'W1 a real turn: the held tick is sent with the wrong size'; find = "                send(&o, heldDelta_);$nl            } else {$nl                settleMisread(&o, heldDelta_);"; put = "                send(&o, delta);$nl            } else {$nl                settleMisread(&o, heldDelta_);" },
    @{ name = 'W1 the held tick stays held after it was settled'; find = "        if (held_ == Held::Reversal) {$nl            held_ = Held::None;"; put = "        if (held_ == Held::Reversal) {" },
    @{ name = 'W1 does not take up the direction after settling'; find = "                settleMisread(&o, heldDelta_);$nl            }$nl            accept(dir, t, sys);"; put = "                settleMisread(&o, heldDelta_);$nl            }" },
    @{ name = 'W1 confirm window is exclusive';       find = 'if (held_ == Held::Reversal && t >= heldAt_ + cfg_.confirmMs) {'; put = 'if (held_ == Held::Reversal && t > heldAt_ + cfg_.confirmMs) {' },
    @{ name = 'W1 never settles a held tick by time'; find = 'if (held_ == Held::Reversal && t >= heldAt_ + cfg_.confirmMs) {'; put = 'if (false) {' },
    @{ name = 'W1 keeps the direction after a dropped tick'; find = "                hasDir_ = false;$nl                *settled = Settled::WheelDropped;"; put = "                *settled = Settled::WheelDropped;" },

    # turning a misread tick round
    @{ name = 'a misread tick is dropped although turning is on'; find = "        if (cfg_.wheelFlip) {$nl            o->settled = Settled::WheelFlipped;"; put = "        if (false) {$nl            o->settled = Settled::WheelFlipped;" },
    @{ name = 'a misread tick is turned although turning is off'; find = "        if (cfg_.wheelFlip) {$nl            o->settled = Settled::WheelFlipped;"; put = "        if (true) {$nl            o->settled = Settled::WheelFlipped;" },
    @{ name = 'a turned tick goes out the wrong way round'; find = 'send(o, -delta);';                put = 'send(o, delta);' },
    @{ name = 'a misread last tick is dropped although turning is on'; find = "            if (cfg_.wheelFlip) {$nl                // The last step of the stroke"; put = "            if (false) {$nl                // The last step of the stroke" },
    @{ name = 'a turned last tick goes out the wrong way round'; find = 'emit->delta = -heldDelta_;'; put = 'emit->delta = heldDelta_;' },
    @{ name = 'a turned last tick does not count as part of the stroke'; find = "                *settled = Settled::WheelFlipped;$nl                accept(dir_, heldAt_, heldSys_);"; put = "                *settled = Settled::WheelFlipped;" },

    # W2: the first ticks of a stroke
    @{ name = 'W2 the first tick is not held';        find = 'if (cfg_.firstMs > 0) {';             put = 'if (false) {' },
    @{ name = 'W2 a lone first tick is never sent';   find = 'if (held_ == Held::First && t >= heldAt_ + cfg_.firstMs) {'; put = 'if (false) {' },
    @{ name = 'W2 first tick window is exclusive';    find = 'if (held_ == Held::First && t >= heldAt_ + cfg_.firstMs) {'; put = 'if (held_ == Held::First && t > heldAt_ + cfg_.firstMs) {' },
    @{ name = 'W2 the stroke counts from the late delivery'; find = 'accept(sign(heldDelta_), heldAt_, heldSys_);'; put = 'accept(sign(heldDelta_), heldAt_ + cfg_.firstMs, heldSys_);' },
    @{ name = 'W2 an agreeing second tick does not release the first'; find = "                held_ = Held::None;$nl                o.settled = Settled::WheelStarted;$nl                send(&o, heldDelta_);$nl                accept(dir, t, sys);"; put = "                held_ = Held::None;$nl                o.settled = Settled::WheelStarted;$nl                accept(dir, t, sys);" },
    @{ name = 'W2 the second of a disagreeing pair is let through'; find = "            o.block = true;$nl            o.verdict = Verdict::WheelFirst;$nl            return o;$nl        }$nl        if (held_ == Held::Pair) {"; put = "            o.block = false;$nl            o.verdict = Verdict::WheelFirst;$nl            return o;$nl        }$nl        if (held_ == Held::Pair) {" },
    @{ name = 'W2 the third tick blames the wrong one of the pair'; find = 'const int right = dir == sign(heldDelta_) ? heldDelta_ : heldDelta2_;'; put = 'const int right = dir == sign(heldDelta_) ? heldDelta2_ : heldDelta_;' },
    @{ name = 'W2 the third tick sends only one of the pair'; find = "            send(&o, right);$nl            settleMisread(&o, wrong);"; put = "            settleMisread(&o, wrong);" },
    @{ name = 'W2 a pair with no third tick loses its second tick'; find = "        if (held_ == Held::Pair) {$nl            held_ = Held::First;"; put = "        if (false) {$nl            held_ = Held::First;" },
    @{ name = 'W2 a pair is timed from its first tick'; find = 'if (held_ == Held::Pair && t >= heldAt2_ + cfg_.firstMs) {'; put = 'if (held_ == Held::Pair && t >= heldAt_ + cfg_.firstMs) {' },
    @{ name = 'W2 a pair is never settled by time';   find = 'if (held_ == Held::Pair && t >= heldAt2_ + cfg_.firstMs) {'; put = 'if (false) {' },
    @{ name = 'W2 the wake-up time of a pair is wrong'; find = 'if (held_ == Held::Pair && heldAt2_ + cfg_.firstMs < d) d = heldAt2_ + cfg_.firstMs;'; put = 'if (held_ == Held::Pair && heldAt_ + cfg_.firstMs < d) d = heldAt_ + cfg_.firstMs;' },
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

    # which mouse (whichmouse.h)
    @{ file = 'whichmouse.h'; name = 'a raw record with no device is kept';          find = '        if (!device) return;'; put = '' },
    @{ file = 'whichmouse.h'; name = 'the newest matching record is used first';     find = 'if (best < 0 || s.t < slots_[best].t) best = i;'; put = 'if (best < 0 || s.t > slots_[best].t) best = i;' },
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
foreach ($name in 'filter.h', 'whichmouse.h') {
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
