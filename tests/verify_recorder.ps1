<#
Checks the recorder against a mouse whose faults are known. inject.exe sends a
scripted pattern (so many clicks, so many of them re-firing; so many wheel
ticks, so many of them reversed) and the three tests must report exactly that:
first with the guard off (faults counted), then with it on (faults dropped).

It runs its own copy of fly-by-wire on another port with a throwaway data
folder, so real recordings are not touched. It takes over the mouse for about
30 seconds.

  verify_recorder.ps1        exit code 0 when every count matches
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$port = 8497
$data = Join-Path $env:TEMP ('fbw-verify-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$base = "http://127.0.0.1:$port"
$headers = @{ 'X-Fly-By-Wire' = '1' }

Add-Type -Namespace FbwVerify -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool ClipCursor(IntPtr rect);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
'@

# Asks fly-by-wire to quit the clean way (it flushes and unhooks), then waits.
function Stop-Guard($Process) {
    $h = [FbwVerify.Win]::FindWindowW('fly-by-wire', 'fly-by-wire')
    if ($h -ne [IntPtr]::Zero) { [void][FbwVerify.Win]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
    if (-not $Process.WaitForExit(5000)) { Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue }
}

function Get-State { Invoke-RestMethod -Uri "$base/api/state" -TimeoutSec 5 }
function Send-Post([string]$Path, $Body) {
    Invoke-RestMethod -Uri "$base$Path" -Method Post -Headers $headers -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 5 | Out-Null
}

function Invoke-Test([string]$Name, [int]$Target, [int]$Every) {
    Send-Post '/api/test' @{ action = 'start'; test = $Name; target = $Target }
    & (Join-Path $build 'inject.exe') $Name $Target $Every | Out-Null
    $deadline = (Get-Date).AddSeconds(8)
    do { Start-Sleep -Milliseconds 200; $s = Get-State } while ($s.test.active -and (Get-Date) -lt $deadline)
    if ($s.test.active) { Send-Post '/api/test' @{ action = 'stop' }; $s = Get-State }
    $s.runs[-1]
}

$failed = 0
function Expect([string]$What, $Run, [int]$Good, [int]$Wrong, [int]$Caught) {
    $ok = $Run.good -eq $Good -and $Run.wrong -eq $Wrong -and $Run.caught -eq $Caught
    if (-not $ok) { $script:failed++ }
    $gaps = if ($Run.wrongGaps.Count) { ' gaps ms: ' + (($Run.wrongGaps | ForEach-Object { '{0:N1}' -f $_ }) -join ' ') } else { '' }
    '{0}  {1,-34} counted {2,3} wrong {3} dropped {4}   (want {5} / {6} / {7}){8}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $What, $Run.good, $Run.wrong, $Run.caught, $Good, $Wrong, $Caught, $gaps
}

$proc = Start-Process -FilePath (Join-Path $build 'fly-by-wire.exe') -PassThru -ArgumentList @('--count-injected', '--port', $port, '--data', "`"$data`"", '--web', "`"$(Join-Path $root 'web')`"")
try {
    $up = $false
    for ($i = 0; $i -lt 30 -and -not $up; $i++) { Start-Sleep -Milliseconds 200; try { [void](Get-State); $up = $true } catch {} }
    if (-not $up) { throw "fly-by-wire did not answer on port $port (is another copy running? only one may run)" }

    Send-Post '/api/config' @{ enabled = $false }
    Expect 'guard off: 20 clicks, 4 re-fire'  (Invoke-Test 'clicks' 20 5) 20 4 0
    Expect 'guard off: 40 up, 3 reversed'     (Invoke-Test 'up' 40 10)    40 3 0
    Expect 'guard off: 40 down, 3 reversed'   (Invoke-Test 'down' 40 10)  40 3 0

    Send-Post '/api/config' @{ enabled = $true; clickGuard = $true; bounceLeft = 40; holdGuard = $false; wheelGuard = $true; reverseMs = 150; confirmMs = 200 }
    Expect 'guard on:  20 clicks, 4 re-fire'  (Invoke-Test 'clicks' 20 5) 20 0 4
    Expect 'guard on:  40 up, 3 reversed'     (Invoke-Test 'up' 40 10)    40 0 3
    Expect 'guard on:  40 down, 3 reversed'   (Invoke-Test 'down' 40 10)  40 0 3

    $s = Get-State
    'hook: {0} calls, {1} lost records, {2} send failures, {3} reinstalls' -f $s.hook.calls, $s.hook.lost, $s.hook.sendFailures, $s.hook.rehooks
    Stop-Guard $proc
    $log = @(Get-Content (Get-ChildItem $data -Filter 'events-*.csv').FullName)
    $bench = @(Get-Content (Join-Path $data 'benchmarks.jsonl'))
    'recording: {0} lines, benchmark records: {1}' -f $log.Count, $bench.Count
    if ($bench.Count -ne 6) { $failed++; 'FAIL  expected 6 benchmark records' }
    # 2 click runs of 20 clicks + 4 re-fires (2 events each), 4 wheel runs of 40 + 3 ticks, 2 header lines
    $sent = 2 * (20 + 4) * 2 + 4 * (40 + 3) + 2
    if ($log.Count -lt $sent) { $failed++; "FAIL  the recording has $($log.Count) lines, $sent were sent" }
} catch {
    $failed++
    'FAIL  ' + $_.Exception.Message
} finally {
    [void][FbwVerify.Win]::ClipCursor([IntPtr]::Zero)
    if (-not $proc.HasExited) { Stop-Guard $proc }
    Start-Sleep -Milliseconds 300
    Remove-Item -LiteralPath $data -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failed) { "$failed check(s) failed"; exit 1 } else { 'recorder counts match the scripted mouse' }
