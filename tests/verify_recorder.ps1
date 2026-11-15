<#
Checks the recorder against a mouse whose faults are known. inject.exe sends a
scripted pattern (so many clicks, so many of them re-firing; so many wheel
ticks, so many of them reversed) and the three tests must report exactly that:
first with the guard off (faults counted), then with it on (faults dropped).

It runs its own copy of fly-by-wire on another port with a throwaway data
folder, so real recordings are not touched. It takes over the mouse for about
40 seconds.

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
'@

# Asks fly-by-wire to quit the clean way (it flushes and unhooks), then waits.
function Stop-Guard($Process) {
    & (Join-Path $build 'fly-by-wire.exe') --quit
    if (-not $Process.WaitForExit(5000)) { Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue }
}

function Get-State { Invoke-RestMethod -Uri "$base/api/state" -TimeoutSec 5 }
function Send-Post([string]$Path, $Body) {
    Invoke-RestMethod -Uri "$base$Path" -Method Post -Headers $headers -ContentType 'application/json' -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 5 | Out-Null
}

function Invoke-Test([string]$Name, [int]$Target, [int]$Every) {
    # The scripted mouse reports when its own timing slipped (another program's
    # hook or the scheduler held it up); such a run says nothing about the
    # recorder, so it is done once more.
    foreach ($try in 1, 2) {
        Start-Sleep -Milliseconds 600       # a fresh start, apart from the run before
        Send-Post '/api/test' @{ action = 'start'; test = $Name; target = $Target }
        $sent = & (Join-Path $build 'inject.exe') $Name $Target $Every | Out-String
        if ($sent -notmatch 'slipped' -or $try -eq 2) { break }
        "  (the scripted mouse slipped: $($sent.Trim() -replace "`r?`n", ' '); once more)" | Out-Host
        # The slipped run is still recorded: count it in what the checks at the end expect.
        $script:retries++
        if ($sent -match 'sent \w+: (\d+) with (\d+) fault') {
            $events = [int]$Matches[1] + [int]$Matches[2]
            $script:extraEvents += $(if ($Name -eq 'clicks') { 2 * $events } else { $events })
        }
        $deadline = (Get-Date).AddSeconds(8)
        do { Start-Sleep -Milliseconds 200; $s = Get-State } while ($s.test.active -and (Get-Date) -lt $deadline)
        if ($s.test.active) { Send-Post '/api/test' @{ action = 'stop' } }
    }
    $deadline = (Get-Date).AddSeconds(8)
    do { Start-Sleep -Milliseconds 200; $s = Get-State } while ($s.test.active -and (Get-Date) -lt $deadline)
    if ($s.test.active) { Send-Post '/api/test' @{ action = 'stop' }; $s = Get-State }
    $s.runs[-1]
}

$failed = 0
$retries = 0
$extraEvents = 0
function Expect([string]$What, $Run, [int]$Good, [int]$Wrong, [int]$Caught) {
    $ok = $Run.good -eq $Good -and $Run.wrong -eq $Wrong -and $Run.caught -eq $Caught
    if (-not $ok) { $script:failed++ }
    $gaps = if ($Run.wrongGaps.Count) { ' gaps ms: ' + (($Run.wrongGaps | ForEach-Object { '{0:N1}' -f $_ }) -join ' ') } else { '' }
    '{0}  {1,-34} counted {2,3} wrong {3} dropped {4}   (want {5} / {6} / {7}){8}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $What, $Run.good, $Run.wrong, $Run.caught, $Good, $Wrong, $Caught, $gaps
}

# A settings file from the start, so the copy under test does not take this
# for a first run: it would open its page in the browser and put itself in
# Start with Windows in place of the copy that runs there.
New-Item -ItemType Directory -Force -Path $data | Out-Null
Set-Content -LiteralPath (Join-Path $data 'config.json') -Value '{"enabled": false}'
$proc = Start-Process -FilePath (Join-Path $build 'fly-by-wire.exe') -PassThru -ArgumentList @('--count-injected', '--port', $port, '--data', "`"$data`"", '--web', "`"$(Join-Path $root 'web')`"")
try {
    $up = $false
    for ($i = 0; $i -lt 30 -and -not $up; $i++) { Start-Sleep -Milliseconds 200; try { [void](Get-State); $up = $true } catch {} }
    if (-not $up) { throw "fly-by-wire did not answer on port $port (is another copy running? only one may run)" }

    Send-Post '/api/config' @{ enabled = $false }
    Expect 'guard off: 20 clicks, 4 re-fire'  (Invoke-Test 'clicks' 20 5) 20 4 0
    Expect 'guard off: 40 up, 3 reversed'     (Invoke-Test 'up' 40 10)    40 3 0
    Expect 'guard off: 40 down, 3 reversed'   (Invoke-Test 'down' 40 10)  40 3 0

    # Wheel rule with its extras off: a stray tick is dropped. The direction is
    # remembered, and each run goes the other way from the one before, so its
    # first tick waits for its second; with nothing sent, that first notch is lost.
    Send-Post '/api/config' @{ enabled = $true; clickGuard = $true; bounceLeft = 40; holdLeft = 0; blipMs = 0; wheelGuard = $true; wheelResend = $false; wheelFlip = $false; firstMs = 0; reverseMs = 150; confirmMs = 200; turnMs = 1000 }
    Expect 'guard on:  20 clicks, 4 re-fire'  (Invoke-Test 'clicks' 20 5) 20 0 4
    Expect 'dropping:  40 up, 3 reversed'     (Invoke-Test 'up' 40 10)    39 0 3
    Expect 'dropping:  40 down, 3 reversed'   (Invoke-Test 'down' 40 10)  39 0 3

    # Wheel rule as fitted to the Rival 500: a stray tick is turned round, so it
    # counts as a good one, and the first tick of each run is sent once its second agrees.
    Send-Post '/api/config' @{ wheelResend = $true; wheelFlip = $true; firstMs = 40; reverseMs = 70; confirmMs = 200; turnMs = 1000 }
    Expect 'turning:   40 up, 3 reversed'     (Invoke-Test 'up' 40 10)    43 0 3
    Expect 'turning:   40 down, 3 reversed'   (Invoke-Test 'down' 40 10)  43 0 3

    # Settings from the page are made sane as they are at start-up: a reversal
    # window above the confirm window pulls the confirm window (and the turn
    # wait) up with it, or a slow real turn would be turned round notch by notch.
    Send-Post '/api/config' @{ reverseMs = 300 }
    $s = Get-State
    if ($s.config.confirmMs -ne 300 -or $s.config.turnMs -lt 300) { $failed++; "FAIL  a reversal window of 300 left the confirm window at $($s.config.confirmMs) and the turn wait at $($s.config.turnMs)" }
    else { 'PASS  a reversal window above the confirm window pulls it up' }
    Send-Post '/api/config' @{ reverseMs = 70; confirmMs = 200 }

    # A bounced click on Start must not restart the test, and one that lands on
    # Stop right after must not end it. (First a pause: a start right after a
    # test ended by itself is ignored too, which is checked further down.)
    Start-Sleep -Milliseconds 600
    Send-Post '/api/test' @{ action = 'start'; test = 'up'; target = 5 }
    Start-Sleep -Milliseconds 300
    Send-Post '/api/test' @{ action = 'start'; test = 'up'; target = 5 }
    $s = Get-State
    if ($s.test.seconds -lt 0.25) { $failed++; 'FAIL  a second start right after the first began the test anew' } else { 'PASS  a second start right after the first is ignored' }
    Send-Post '/api/test' @{ action = 'stop' }
    $s = Get-State
    if (-not $s.test.active) { $failed++; 'FAIL  a stop right after the start ended the test' } else { 'PASS  a stop right after the start is ignored' }
    Start-Sleep -Milliseconds 400
    Send-Post '/api/test' @{ action = 'stop' }
    $s = Get-State
    if ($s.test.active) { $failed++; 'FAIL  a later stop did not end the test' } else { 'PASS  a later stop ends the test' }
    # The page puts Start where Stop was, so a bounced click on Stop lands on Start.
    Send-Post '/api/test' @{ action = 'start'; test = 'up'; target = 5 }
    $s = Get-State
    if ($s.test.active) { $failed++; 'FAIL  a start right after a stop began a new test' } else { 'PASS  a start right after a stop is ignored' }
    Start-Sleep -Milliseconds 600
    Send-Post '/api/test' @{ action = 'start'; test = 'up'; target = 5 }
    $s = Get-State
    if (-not $s.test.active) { $failed++; 'FAIL  a start well after a stop did not begin a test' } else { 'PASS  a start well after a stop begins a test' }
    Start-Sleep -Milliseconds 600
    Send-Post '/api/test' @{ action = 'stop' }

    $s = Get-State
    'hook: {0} calls, {1} lost records, {2} send failures, {3} reinstalls, {4} slow, {5} not trusted, slowest {6:N3} ms' -f $s.hook.calls, $s.hook.lost, $s.hook.sendFailures, $s.hook.rehooks, $s.hook.slow, $s.hook.stalls, ($s.hook.maxOwnUs / 1000)
    if ($s.hook.stalls -ne 0) { $failed++; 'FAIL  a callback ran so long that its verdict was not trusted' }
    if (-not $s.build) { $failed++; 'FAIL  the state carries no build stamp' }
    $page = (Invoke-WebRequest -Uri "$base/" -UseBasicParsing -TimeoutSec 5).Content
    if ($page -notmatch [regex]::Escape("var BUILD = '$($s.build)'")) { $failed++; 'FAIL  the page does not carry the build stamp of the program' } else { 'PASS  page and program carry the same build stamp' }

    # The runs must still be there after a restart, each with the same numbers.
    $before = ($s.runs | ConvertTo-Json -Depth 6 -Compress)
    Stop-Guard $proc
    $log = @(Get-Content (Get-ChildItem $data -Filter 'events-*.csv').FullName)
    $bench = @(Get-Content (Join-Path $data 'benchmarks.jsonl'))
    'recording: {0} lines, benchmark records: {1}' -f $log.Count, $bench.Count
    if ($retries) { "($retries run(s) done again after the scripted mouse slipped)" }
    if ($bench.Count -ne 8 + $retries) { $failed++; "FAIL  expected $(8 + $retries) benchmark records" }
    $header = $log[0].Split(',')
    # 2 click runs of 20 clicks + 4 re-fires (2 events each) and 6 wheel runs of 40 + 3 ticks:
    # exactly that many rows must carry an event of the scripted mouse.
    $sent = 2 * (20 + 4) * 2 + 6 * (40 + 3) + $extraEvents
    $sourceIndex = [array]::IndexOf($header, 'source')
    $kindIndex = [array]::IndexOf($header, 'kind')
    $mouseRows = @($log | Where-Object { $_ -match '^\d' } | ForEach-Object { , $_.Split(',') } | Where-Object { $_[$sourceIndex] -eq '0' -and $_[$kindIndex] -ne '3' })
    if ($mouseRows.Count -ne $sent) { $failed++; "FAIL  the recording has $($mouseRows.Count) rows from the scripted mouse, $sent were sent" } else { "PASS  the recording has all $sent events of the scripted mouse" }
    foreach ($col in 'sys_ms', 'lag_ms', 'flags', 'vetoed', 'stalled', 'device', 'device_how', 'judged') {
        if ($header -notcontains $col) { $failed++; "FAIL  the recording has no $col column" }
    }
    $rows = $log | Where-Object { $_ -match '^\d' } | ForEach-Object { , $_.Split(',') }
    $lagIndex = [array]::IndexOf($header, 'lag_ms')
    $lags = @($rows | Where-Object { $_[2] -ne '3' } | ForEach-Object { [int]$_[$lagIndex] })
    'callback lag on the coarse clock: {0} events, largest {1} ms' -f $lags.Count, ($lags | Measure-Object -Maximum).Maximum
    if (@($log | Where-Object { $_ -like '# config *' }).Count -lt 2) { $failed++; 'FAIL  settings changes are not written into the recording' }

    $proc = Start-Process -FilePath (Join-Path $build 'fly-by-wire.exe') -PassThru -ArgumentList @('--count-injected', '--port', $port, '--data', "`"$data`"", '--web', "`"$(Join-Path $root 'web')`"")
    $up = $false
    for ($i = 0; $i -lt 30 -and -not $up; $i++) { Start-Sleep -Milliseconds 200; try { $s = Get-State; $up = $true } catch {} }
    if (-not $up) { $failed++; 'FAIL  did not come back after a restart' }
    elseif ($s.runs.Count -ne 8 + $retries) { $failed++; "FAIL  after a restart the page shows $($s.runs.Count) runs, $(8 + $retries) were recorded" }
    elseif (($s.runs | ConvertTo-Json -Depth 6 -Compress) -ne $before) { $failed++; 'FAIL  a run came back from disk with other numbers' }
    else { "PASS  the $(8 + $retries) runs are shown after a restart, each with the same numbers" }
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
