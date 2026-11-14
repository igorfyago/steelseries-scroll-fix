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

$mutants = @(
    @{ name = 'C1 threshold is inclusive';            find = '(t - s.lastUp) < cfg_.bounceMs[b]';   put = '(t - s.lastUp) <= cfg_.bounceMs[b]' },
    @{ name = 'C1 forgets to drop the release';       find = "                s.swallow = true;`r`n                o.block = true;`r`n                o.verdict = Verdict::BouncePress;"; put = "                o.block = true;`r`n                o.verdict = Verdict::BouncePress;" },
    @{ name = 'C1 passes the bounce press';           find = "                s.swallow = true;`r`n                o.block = true;"; put = "                s.swallow = true;`r`n                o.block = false;" },
    @{ name = 'C1 ignores dropped releases for the gap'; find = "        s.lastUp = t;`r`n        s.hasUp = true;"; put = "        if (!s.swallow) s.lastUp = t;`r`n        s.hasUp = true;" },
    @{ name = 'C1 applies to the first press ever';   find = 'cfg_.clickGuard && s.hasUp && (t - s.lastUp)'; put = 'cfg_.clickGuard && (t - s.lastUp)' },
    @{ name = 'C1 ignores the on/off switch';         find = 'cfg_.clickGuard && s.hasUp && (t - s.lastUp)'; put = 's.hasUp && (t - s.lastUp)' },
    @{ name = 'C2 doubles the hold';                  find = 's.upDeadline = t + cfg_.holdMs;';     put = 's.upDeadline = t + cfg_.holdMs * 2;' },
    @{ name = 'C2 re-press does not cancel the release'; find = "                s.pendingUp = false;`r`n                o.block = true;`r`n                o.verdict = Verdict::BridgedPress;"; put = "                o.block = true;`r`n                o.verdict = Verdict::BridgedPress;" },
    @{ name = 'C2 passes the bridging press';         find = "                o.block = true;`r`n                o.verdict = Verdict::BridgedPress;"; put = "                o.block = false;`r`n                o.verdict = Verdict::BridgedPress;" },
    @{ name = 'C2 holds releases the app never saw pressed'; find = '        } else if (!s.appDown) {'; put = '        } else if (false) {' },
    @{ name = 'C2 release deadline is exclusive';     find = 's.pendingUp && t >= s.upDeadline';    put = 's.pendingUp && t > s.upDeadline' },
    @{ name = 'C2 flush forgets the button';          find = "                emit->kind = Emit::ButtonUp;`r`n                emit->button = b;`r`n                return true;`r`n            }`r`n        }`r`n        held_ = false;"; put = "                emit->kind = Emit::ButtonUp;`r`n                emit->button = 0;`r`n                return false;`r`n            }`r`n        }`r`n        held_ = false;" },
    @{ name = 'W1 window is inclusive';               find = '(t - lastAccept_) < cfg_.reverseMs';  put = '(t - lastAccept_) <= cfg_.reverseMs' },
    @{ name = 'W1 confirmed tick is not sent';        find = '            o.hasEmit = true;';           put = '            o.hasEmit = false;' },
    @{ name = 'W1 confirmed tick is sent with the wrong size'; find = 'o.emit.delta = heldDelta_;'; put = 'o.emit.delta = delta;' },
    @{ name = 'W1 stray tick stays held after the real direction resumes'; find = "                held_ = false;`r`n                o.settled = Settled::WheelDropped;`r`n            }`r`n            accept(dir, t);"; put = "                o.settled = Settled::WheelDropped;`r`n            }`r`n            accept(dir, t);" },
    @{ name = 'W1 passes the held tick';              find = "            o.block = true;`r`n            o.verdict = Verdict::WheelHeld;"; put = "            o.block = false;`r`n            o.verdict = Verdict::WheelHeld;" },
    @{ name = 'W1 ignores the on/off switch';         find = 'if (!cfg_.wheelGuard || !hasTick_) {'; put = 'if (!hasTick_) {' },
    @{ name = 'W1 holds the very first tick';         find = 'if (!cfg_.wheelGuard || !hasTick_) {'; put = 'if (!cfg_.wheelGuard) {' },
    @{ name = 'W1 confirm window is exclusive';       find = 'if (held_ && t >= heldAt_ + cfg_.confirmMs) {'; put = 'if (held_ && t > heldAt_ + cfg_.confirmMs) {' },
    @{ name = 'W1 never expires a held tick';         find = 'if (held_ && t >= heldAt_ + cfg_.confirmMs) {'; put = 'if (false) {' },
    @{ name = 'W1 keeps the old direction after a real reversal'; find = "            o.emit.delta = heldDelta_;`r`n            accept(dir, t);"; put = "            o.emit.delta = heldDelta_;" },
    @{ name = 'busy() forgets hidden presses';        find = 'if (btn_[b].pendingUp || btn_[b].swallow) return true;'; put = 'if (btn_[b].pendingUp) return true;' },
    @{ name = 'unknown button is indexed';            find = 'if (b < 0 || b >= kButtons) return o;'; put = 'if (b < -9) return o;' }
)

$original = [IO.File]::ReadAllText((Join-Path $src 'filter.h'))
if ($original -notmatch "`r`n") { $original = $original -replace "`n", "`r`n" }

$survived = @()
$i = 0
foreach ($m in $mutants) {
    $i++
    $count = ([regex]::Matches($original, [regex]::Escape($m.find))).Count
    if ($count -ne 1) { throw "mutant '$($m.name)': the text to break occurs $count times, expected exactly 1" }
    $dir = Join-Path $work "m$i"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir 'filter.h'), $original.Replace($m.find, $m.put))
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
