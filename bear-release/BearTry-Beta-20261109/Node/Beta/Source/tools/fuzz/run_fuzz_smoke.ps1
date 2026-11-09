# Bounded ASan fuzz smoke + per-input regression runner (Line 3).
#
# For every target executable produced by build_asan.ps1 this script:
#   1. runs a bounded deterministic campaign (fixed seed, iteration/time budget);
#   2. runs every corpus file through --one-shot in its own child process with a
#      wall-clock timeout (single-case hang detection, ASan crash isolation);
#   3. records exe/corpus SHA-256, exit codes, durations, ASan report presence and
#      copies raw logs into evidence/line3/<run-id>/.
#
# Exit code: 0 only if every campaign and every one-shot input completed without
# a crash, timeout or ASan report within the configured budget.
#
# Usage:
#   pwsh tools/fuzz/run_fuzz_smoke.ps1
#   pwsh tools/fuzz/run_fuzz_smoke.ps1 -Iterations 500 -MaxSeconds 30 -RunId smoke-01

param(
    [string]$Repo = 'C:\DHW\Gplus\src\grinplusplus',
    [string]$BuildDir = 'C:\DHW\.tmp\asan-line3\fuzz',
    [string]$OutRoot = 'C:\DHW\.tmp\asan-line3\runs',
    [string]$EvidenceRoot = 'C:\DHW\Gplus\src\grinplusplus\evidence\line3',
    [string]$CorpusRoot = '',
    [int]$Iterations = 300,
    [int]$Seed = 20261103,
    [double]$MaxSeconds = 60,
    [int]$OneShotTimeoutSec = 30,
    [string[]]$Targets = @('bytebuffer', 'p2p_body', 'segment', 'zip', 'nrd_rules'),
    [string]$RunId = (Get-Date -Format 'yyyyMMdd-HHmmss')
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
if (-not $CorpusRoot) { $CorpusRoot = Join-Path $Repo 'tests\fuzz\corpus' }
# -File invocation binds "a,b" as one string; accept both comma-separated and repeats.
$Targets = @($Targets | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$runDir = Join-Path $OutRoot $RunId
if (Test-Path -LiteralPath $runDir) { throw "run already exists: $runDir" }
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$staging = Join-Path $runDir 'zip-staging'
New-Item -ItemType Directory -Force -Path $staging | Out-Null
$env:GRINPP_FUZZ_STAGING = $staging

function Get-Sha([string]$Path) {
    if (Test-Path -LiteralPath $Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
    return $null
}

$manifest = [ordered]@{
    schema        = 'gpp-line3-fuzz-run/v1'
    run_id        = $RunId
    created_utc   = (Get-Date).ToUniversalTime().ToString('o')
    repo          = $Repo
    git_head      = (& git -C $Repo rev-parse HEAD).Trim()
    git_status    = @(& git -C $Repo status --porcelain)
    seed          = $Seed
    iterations    = $Iterations
    max_seconds   = $MaxSeconds
    one_shot_timeout_sec = $OneShotTimeoutSec
    environment   = [ordered]@{
        os         = [Environment]::OSVersion.VersionString
        asan_staging = $staging
        note       = 'ASan runtime DLL is copied next to the harness exe by build_asan.ps1'
    }
    targets       = [ordered]@{}
    result        = 'UNKNOWN'
}

$failures = @()

foreach ($target in $Targets) {
    $exe = Join-Path $BuildDir "fuzz_$target.exe"
    if (-not (Test-Path -LiteralPath $exe)) { throw "missing harness: $exe" }
    $corpus = Join-Path $CorpusRoot $target
    if (-not (Test-Path -LiteralPath $corpus)) { throw "missing corpus: $corpus" }

    $targetInfo = [ordered]@{
        exe_sha256 = Get-Sha $exe
        exe        = $exe
        corpus     = $corpus
        corpus_sha256 = (Get-ChildItem -LiteralPath $corpus -File | Sort-Object Name | ForEach-Object {
                $_.Name + ':' + (Get-Sha $_.FullName)
            }) -join ';'
        campaign   = [ordered]@{}
        one_shot   = [ordered]@{}
        crashed    = $false
        timeouts   = 0
        asan_reports = 0
    }

    # 1. Bounded campaign.
    $outDir = Join-Path $runDir "campaign-$target"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $stdout = Join-Path $outDir 'stdout.log'
    $stderr = Join-Path $outDir 'stderr.log'
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -ArgumentList @('--corpus', $corpus, '--out', $outDir,
            '--iterations', "$Iterations", '--seed', "$Seed", '--max-seconds', "$MaxSeconds", '--quiet') `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -NoNewWindow
    $finished = $p.WaitForExit([int](($MaxSeconds + 120) * 1000))
    if (-not $finished) {
        try { & taskkill /PID $p.Id /T /F 2>&1 | Out-Null } catch { }
    }
    $p.WaitForExit()
    $sw.Stop()
    $stdoutText = if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout -Raw } else { '' }
    $stderrText = if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr -Raw } else { '' }
    $asan = ($stderrText -match 'AddressSanitizer') -or ($stdoutText -match 'AddressSanitizer')
    $targetInfo.asan_reports += if ($asan) { 1 } else { 0 }
    if (-not $finished) { $targetInfo.timeouts++ ; $failures += "$target campaign timeout" }
    if ($asan) { $targetInfo.crashed = $true; $failures += "$target campaign ASan report" }
    if ($p.ExitCode -ne 0) { $targetInfo.crashed = $true; $failures += "$target campaign exit $($p.ExitCode)" }
    $targetInfo.campaign = [ordered]@{
        requested_iterations = $Iterations
        exit_code            = $p.ExitCode
        wall_sec             = [Math]::Round($sw.Elapsed.TotalSeconds, 3)
        stdout               = $stdout
        stderr               = $stderr
        asan_report          = $asan
        summary_line         = (($stdoutText -split "`r?`n") | Where-Object { $_ -match '^SUMMARY ' } | Select-Object -Last 1)
        current_case         = (Join-Path $outDir 'current_case.bin')
        last_case            = (Join-Path $outDir 'last_case.txt')
    }

    # 2. One-shot regression over every corpus input (child process isolation).
    $files = Get-ChildItem -LiteralPath $corpus -File | Sort-Object Name
    $results = @()
    foreach ($f in $files) {
        $caseOut = Join-Path $runDir ("oneshot-$target-" + $f.Name + ".log")
        $psw = [System.Diagnostics.Stopwatch]::StartNew()
        $qp = Start-Process -FilePath $exe -ArgumentList @('--one-shot', $f.FullName) `
            -RedirectStandardOutput $caseOut -RedirectStandardError ($caseOut + '.err') -PassThru -NoNewWindow
        $qfinished = $qp.WaitForExit($OneShotTimeoutSec * 1000)
        if (-not $qfinished) {
            try { & taskkill /PID $qp.Id /T /F 2>&1 | Out-Null } catch { }
            $targetInfo.timeouts++
            $failures += "$target one-shot timeout: $($f.Name)"
        }
        $qp.WaitForExit()
        $psw.Stop()
        $caseText = if (Test-Path -LiteralPath $caseOut) { Get-Content -LiteralPath $caseOut -Raw } else { '' }
        $caseErr = if (Test-Path -LiteralPath ($caseOut + '.err')) { Get-Content -LiteralPath ($caseOut + '.err') -Raw } else { '' }
        $caseAsan = ($caseErr -match 'AddressSanitizer') -or ($caseText -match 'AddressSanitizer')
        if ($caseAsan) { $targetInfo.asan_reports++; $targetInfo.crashed = $true; $failures += "$target one-shot ASan: $($f.Name)" }
        if ($qp.ExitCode -ne 0) { $targetInfo.crashed = $true; $failures += "$target one-shot exit $($qp.ExitCode): $($f.Name)" }
        $outcome = (($caseText -split "`r?`n") | Where-Object { $_ -match 'outcome=' } | Select-Object -Last 1)
        $results += [ordered]@{
            input      = $f.Name
            bytes      = $f.Length
            exit_code  = $qp.ExitCode
            wall_sec   = [Math]::Round($psw.Elapsed.TotalSeconds, 3)
            outcome    = $outcome
            asan       = $caseAsan
            timed_out  = -not $qfinished
        }
    }
    $targetInfo.one_shot = @($results)
    $manifest.targets[$target] = $targetInfo
    Write-Host ("target {0}: campaign exit={1} asan={2}; one-shot {3} inputs, timeouts={4}, asan={5}" -f `
        $target, $p.ExitCode, $asan, $results.Count, $targetInfo.timeouts, $targetInfo.asan_reports)
}

$manifest.failure_reasons = @($failures)
$manifest.result = if ($failures.Count -eq 0) { 'PASS' } else { 'FAIL' }
$manifestPath = Join-Path $runDir 'manifest.json'
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding utf8

$evDir = Join-Path $EvidenceRoot $RunId
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $evDir 'manifest.json') -Force
Copy-Item -Path (Join-Path $runDir '*') -Destination $evDir -Recurse -Force -ErrorAction SilentlyContinue
# Do not keep the zip staging payloads as evidence.
$evStaging = Join-Path $evDir 'zip-staging'
if (Test-Path -LiteralPath $evStaging) { Remove-Item -LiteralPath $evStaging -Recurse -Force }

Write-Host ''
Write-Host "fuzz smoke result : $($manifest.result)"
Write-Host "seed/iterations   : $Seed / $Iterations (max $MaxSeconds s per target)"
Write-Host "manifest          : $manifestPath"
Write-Host "evidence copy     : $evDir"
if ($failures.Count -gt 0) {
    Write-Host 'failures:'
    $failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
exit 0
