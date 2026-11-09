# Task-8(b) external child-kill harness for NRD rebuild interruption.
#
# No production hook: this script launches the normal Tests.exe child
# ("RebuildKillChild", hidden test), waits until it has mined a real chain and
# written the state marker, force-kills it while it is restarting/rebuilding, then
# launches "RebuildKillRecovery" against the same isolated HOME and requires a
# complete rebuilt index. The child/recovery tests are hidden ([.]) and run only
# through this harness.
#
# Usage:
#   pwsh tools/audit/nrd/rebuild-kill-harness.ps1 -TestsExe <path> -ExpectedTestsSha <sha256>

param(
    [Parameter(Mandatory = $true)][string]$TestsExe,
    [Parameter(Mandatory = $true)][string]$ExpectedTestsSha,
    [string]$RunRoot = 'C:\DHW\.tmp\line2-nrd-kill',
    [string]$EvidenceRoot = 'C:\DHW\Gplus\src\grinplusplus\tools\audit\nrd\evidence\isolated',
    [string]$RunId = ('kill-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [int]$ChildTimeoutSec = 300,
    [int]$KillDelayMs = 300,
    [int]$RecoveryTimeoutSec = 600
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $TestsExe)) { throw "Tests binary not found: $TestsExe" }
$sha = (Get-FileHash -LiteralPath $TestsExe -Algorithm SHA256).Hash
if ($sha -ne $ExpectedTestsSha) { throw "Tests SHA mismatch: expected $ExpectedTestsSha, actual $sha" }

$runDir = Join-Path $RunRoot $RunId
if (Test-Path -LiteralPath $runDir) { throw "run dir already exists: $runDir" }
$homeAbs = Join-Path $runDir 'home'
$tmpAbs = Join-Path $runDir 'tmp'
$cwdAbs = Join-Path $runDir 'cwd'
$outDir = Join-Path $runDir 'out'
New-Item -ItemType Directory -Force -Path $homeAbs, $tmpAbs, $cwdAbs, $outDir | Out-Null

$statePath = Join-Path $runDir 'state.txt'
$orig = @{}
foreach ($k in 'HOMEDRIVE', 'HOMEPATH', 'USERPROFILE', 'HOME', 'TMP', 'TEMP', 'TMPDIR', 'GRINPP_NRD_KILL_STATE', 'GRINPP_NRD_KILL_BACKUP') {
    $orig[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
}

try {
    $env:HOMEDRIVE = 'C:'
    $env:HOMEPATH = $homeAbs
    $env:USERPROFILE = $homeAbs
    $env:HOME = $homeAbs
    $env:TMP = $tmpAbs
    $env:TEMP = $tmpAbs
    $env:TMPDIR = $tmpAbs
    $env:GRINPP_NRD_KILL_STATE = $statePath

    $childOut = Join-Path $outDir 'child.stdout.log'
    $childErr = Join-Path $outDir 'child.stderr.log'
    Write-Host "child: tests=$TestsExe sha=$sha"
    $child = Start-Process -FilePath $TestsExe -ArgumentList 'RebuildKillChild' -WorkingDirectory $cwdAbs `
        -RedirectStandardOutput $childOut -RedirectStandardError $childErr -PassThru -NoNewWindow

    $deadline = (Get-Date).AddSeconds($ChildTimeoutSec)
    while (-not (Test-Path -LiteralPath $statePath)) {
        if ($child.HasExited) { throw "child exited before writing the state marker (exit $($child.ExitCode)); see $childOut" }
        if ((Get-Date) -gt $deadline) { throw "timeout waiting for state marker: $statePath" }
        Start-Sleep -Milliseconds 100
    }

    Start-Sleep -Milliseconds $KillDelayMs
    $killed = -not $child.HasExited
    if ($killed) {
        & taskkill /PID $child.Id /T /F 2>&1 | Out-Null
    }
    Start-Sleep -Milliseconds 300
    Write-Host ("child killed={0} pid={1}" -f $killed, $child.Id)

    # The test runtime wipes HOME/.GrinPP/AUTOMATED_TESTING at the next process
    # start, so snapshot the killed child's data dirs for the recovery process.
    $dataDir = Join-Path $homeAbs '.GrinPP\AUTOMATED_TESTING'
    $backupDir = Join-Path $runDir 'backup'
    if (-not (Test-Path -LiteralPath $dataDir)) { throw "child data dir missing: $dataDir" }
    if (Test-Path -LiteralPath $backupDir) { Remove-Item -LiteralPath $backupDir -Recurse -Force }
    Copy-Item -LiteralPath $dataDir -Destination $backupDir -Recurse -Force
    $env:GRINPP_NRD_KILL_BACKUP = $backupDir

    $recoveryOut = Join-Path $outDir 'recovery.stdout.log'
    $recoveryErr = Join-Path $outDir 'recovery.stderr.log'
    $recovery = Start-Process -FilePath $TestsExe -ArgumentList 'RebuildKillRecovery' -WorkingDirectory $cwdAbs `
        -RedirectStandardOutput $recoveryOut -RedirectStandardError $recoveryErr -PassThru -NoNewWindow
    if (-not $recovery.WaitForExit($RecoveryTimeoutSec * 1000)) {
        & taskkill /PID $recovery.Id /T /F 2>&1 | Out-Null
        throw 'recovery test timed out'
    }

    $recoveryText = Get-Content -LiteralPath $recoveryOut -Raw -ErrorAction SilentlyContinue
    $passed = ($recovery.ExitCode -eq 0) -and ($recoveryText -match 'All tests passed')
    $evidenceDir = Join-Path $EvidenceRoot $RunId
    New-Item -ItemType Directory -Force -Path $evidenceDir | Out-Null
    Copy-Item $childOut, $childErr, $recoveryOut, $recoveryErr -Destination $evidenceDir -Force

    $manifest = [ordered]@{
        schema = 'nrd-rebuild-kill-harness/v1'
        run_id = $RunId
        tests_exe = (Resolve-Path -LiteralPath $TestsExe).Path
        tests_sha256 = $sha
        child_test = 'RebuildKillChild'
        recovery_test = 'RebuildKillRecovery'
        child_killed_while_running = $killed
        child_pid = $child.Id
        recovery_exit = $recovery.ExitCode
        recovery_passed = $passed
        killed_after_state_marker_ms = $KillDelayMs
        isolated_home = $homeAbs
        state_file = $statePath
        note = 'Fault-injected external kill; not an organic network scenario. NRD index is in-memory only; recovery must rebuild a complete index from headers+kernel MMR.'
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidenceDir 'manifest.json') -Encoding utf8

    Write-Host ("recovery exit={0} pass={1}" -f $recovery.ExitCode, $passed)
    Write-Host ("evidence: {0}" -f $evidenceDir)
    if (-not $passed) { exit 1 }
    exit 0
}
finally {
    foreach ($k in $orig.Keys) {
        [Environment]::SetEnvironmentVariable($k, $orig[$k], 'Process')
    }
}
