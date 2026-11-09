# ASan teardown regression (Line 3): zero-test run must be clean.
#
# Reproduces the pre-fix failure mode: an AddressSanitizer-instrumented test
# executable with the test runtime lifetime bug reports a heap-use-after-free on
# process teardown even when the filter matches no test case. The fix moved the
# TestRuntime from a static object to a local object inside main(), so teardown
# must now exit 0 with no ASan report.
#
# The BEFORE evidence is archived at:
#   evidence/line3/asan-testsmini-empty-01   (exit 1, ASan UAF, zero tests)
# This script is the AFTER regression: it must PASS.
#
# Usage:
#   pwsh tools/fuzz/run_asan_teardown_regression.ps1 -ExePath <ASan test exe> -RunId <id>
#
# Exit code 0 = PASS (clean teardown), 1 = FAIL (UAF/crash/timeout/nonzero exit).

param(
    [string]$ExePath = 'C:\DHW\.tmp\asan-line3\fuzz-linked\TestsMini.exe',
    [string]$RunId = ('teardown-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [string]$RunRoot = 'C:\DHW\.tmp\asan-line3\teardown-iso',
    [string]$EvidenceRoot = 'C:\DHW\Gplus\src\grinplusplus\evidence\line3',
    [switch]$KeepBinary
)

$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot '..\build\run-tests-isolated.ps1'
$runner = (Resolve-Path -LiteralPath $runner).Path
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path

$env:ASAN_OPTIONS = 'detect_leaks=0'
$runArgs = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $runner,
    '-ExePath', $ExePath, '-RunId', $RunId, '-RunRoot', $RunRoot,
    '-ExeArgs', '__no_such_test_filter__'
)
if ($KeepBinary) { $runArgs += '-KeepBinary' }
$runnerOutput = & pwsh @runArgs 2>&1
$runnerExit = $LASTEXITCODE
$runnerOutput | Set-Content -LiteralPath (Join-Path $EvidenceRoot "$RunId.runner.log") -Encoding utf8 -ErrorAction SilentlyContinue

$manifestPath = Join-Path $RunRoot ($RunId + '\manifest.json')
if (-not (Test-Path -LiteralPath $manifestPath)) {
    Write-Host "REGRESSION FAIL: runner manifest missing: $manifestPath"
    exit 1
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$instance = $manifest.instances[0]
$stdoutPath = $instance.stdout_path
$stderrPath = $instance.stderr_path
$stdoutText = if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath -Raw } else { '' }
$stderrText = if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath -Raw } else { '' }
$asan = ($stderrText -match 'AddressSanitizer') -or ($stdoutText -match 'AddressSanitizer')

$checks = [ordered]@{
    runner_exit_zero        = ($runnerExit -eq 0)
    manifest_status_pass    = ($manifest.status -eq 'PASS')
    process_exit_zero       = ($instance.exit_code.decimal -eq 0)
    no_asan_report          = (-not $asan)
    zero_tests_ran          = ($stdoutText -match 'No tests ran')
    user_profile_unchanged  = [bool]$manifest.isolation.user_profile_unchanged
    isolated_data_dir       = [bool]$manifest.isolation.test_data_dir_created
    runtime_dll_copied      = (@($manifest.binary.copied_asan_runtime).Count -ge 1)
}
$failed = @($checks.Keys | Where-Object { -not $checks[$_] })
$result = if ($failed.Count -eq 0) { 'PASS' } else { 'FAIL' }

$summary = [ordered]@{
    schema        = 'gpp-line3-asan-teardown-regression/v1'
    run_id        = $RunId
    created_utc   = (Get-Date).ToUniversalTime().ToString('o')
    result        = $result
    exe           = $ExePath
    exe_sha256    = $manifest.binary.source_sha256
    runner_manifest = $manifestPath
    checks        = $checks
    failed_checks = $failed
    asan_report   = $asan
    process_exit  = $instance.exit_code
    stdout        = $stdoutPath
    stderr        = $stderrPath
    note          = 'Pre-fix failing evidence: evidence/line3/asan-testsmini-empty-01 (zero tests, exit 1, ASan heap-use-after-free).'
}
$evDir = Join-Path $EvidenceRoot $RunId
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evDir 'teardown_regression.json') -Encoding utf8

Write-Host ("teardown regression: {0}  exe_sha={1}" -f $result, $manifest.binary.source_sha256)
foreach ($k in $checks.Keys) { Write-Host ("  {0} = {1}" -f $k, $checks[$k]) }
if ($asan) { Write-Host 'ASan report head:'; ($stderrText -split "`r?`n") | Select-Object -First 8 | ForEach-Object { Write-Host $_ } }
Write-Host "evidence: $evDir"
if ($result -eq 'PASS') { exit 0 } else { exit 1 }
