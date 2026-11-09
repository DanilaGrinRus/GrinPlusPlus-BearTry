# Line-2 paired NRD differential report generator.
#
# Runs the EXISTING Tests.exe (never builds) with both the per-case C++ report and
# the real Rust oracle JSON, then emits a paired report: actual C++ vs actual Rust
# only for oracle-computed cases; every not_computed case is listed separately.
#
# Usage (after the Lead grants a Tests binary; no build is performed here):
#   pwsh tools/audit/nrd/paired-differential.ps1 -ExpectedTestsSha <sha256>
#
# Outputs:
#   <repo>/tools/audit/nrd/evidence/cpp-nrd-differential-report-paired-<RunId>.json
#   <repo>/tools/audit/nrd/evidence/paired-differential-report-<RunId>.json
#
# Exit code follows the isolated test run (0 = paired run green).

param(
    [string]$Repo = 'C:\DHW\Gplus\src\grinplusplus',
    [string]$TestsExe = 'C:\DHW\Gplus\src\grinplusplus\bin\Release\Release\Tests.exe',
    [string]$ExpectedTestsSha = '',
    [string]$OracleJson = 'C:\DHW\Gplus\src\grinplusplus\tools\audit\nrd\evidence\rust-oracle-8F10BFD6.json',
    [string]$CasesFile = 'C:\DHW\Gplus\src\grinplusplus\tools\audit\nrd\cases\nrd_cases.json',
    [string]$RunRoot = 'C:\DHW\.tmp\line2-nrd\iso',
    [string]$EvidenceRoot = 'C:\DHW\Gplus\src\grinplusplus\tools\audit\nrd\evidence\isolated',
    [string]$RunId = '',
    [string]$TestFilter = 'Nrd*',
    [int]$TimeoutSec = 900
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $TestsExe)) { throw "Tests binary not found: $TestsExe (no build is performed by this script)" }
if (-not (Test-Path -LiteralPath $OracleJson)) { throw "Rust oracle JSON not found: $OracleJson" }
if (-not (Test-Path -LiteralPath $CasesFile)) { throw "Cases JSON not found: $CasesFile" }
if (-not (Test-Path -LiteralPath (Join-Path $Repo 'tools\build\run-tests-isolated.ps1'))) {
    throw "isolated runner not found under $Repo"
}

$testsSha = (Get-FileHash -LiteralPath $TestsExe -Algorithm SHA256).Hash
if ($ExpectedTestsSha -and ($testsSha -ne $ExpectedTestsSha)) {
    throw "Tests SHA mismatch: expected $ExpectedTestsSha, actual $testsSha"
}

if (-not $RunId) { $RunId = 'nrd-paired-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }

$cppReportPath = Join-Path $Repo "tools\audit\nrd\evidence\cpp-nrd-differential-report-paired-$RunId.json"
$pairedReportPath = Join-Path $Repo "tools\audit\nrd\evidence\paired-differential-report-$RunId.json"

Write-Host "Tests binary : $TestsExe"
Write-Host "Tests SHA-256: $testsSha"
Write-Host "Oracle JSON  : $OracleJson"
Write-Host "Run id       : $RunId"

$env:GRINPP_NRD_CASES = $CasesFile
$env:GRINPP_NRD_RUST_ORACLE = $OracleJson
$env:GRINPP_NRD_REPORT = $cppReportPath

$runner = Join-Path $Repo 'tools\build\run-tests-isolated.ps1'
& pwsh -NoProfile -File $runner -ExePath $TestsExe -RunId $RunId -RunRoot $RunRoot `
    -EvidenceRoot $EvidenceRoot -KeepBinary -TimeoutSec $TimeoutSec -TestArgs $TestFilter
$runExit = $LASTEXITCODE
Remove-Item Env:GRINPP_NRD_RUST_ORACLE -ErrorAction SilentlyContinue
Remove-Item Env:GRINPP_NRD_REPORT -ErrorAction SilentlyContinue
Remove-Item Env:GRINPP_NRD_CASES -ErrorAction SilentlyContinue
Remove-Item Env:GRINPP_NRD_CASES -ErrorAction SilentlyContinue

$oracle = Get-Content -LiteralPath $OracleJson -Raw | ConvertFrom-Json
$cases = (Get-Content -LiteralPath $CasesFile -Raw | ConvertFrom-Json).cases
$expectedCppById = @{}
foreach ($case in $cases) { $expectedCppById[$case.id] = $case.expect.cpp_category }

$cppById = @{}
if (Test-Path -LiteralPath $cppReportPath) {
    foreach ($entry in (Get-Content -LiteralPath $cppReportPath -Raw | ConvertFrom-Json)) {
        $cppById[$entry.id] = $entry
    }
}

$rows = @()
$computed = 0
$notComputed = 0
$pairedMismatches = 0
$pairedComputedMismatchIds = @()
$notComputedIds = @()

foreach ($oracleCase in $oracle.cases) {
    $row = [ordered]@{
        id = $oracleCase.id
        oracle_status = $oracleCase.status
        actual_rust_category = $oracleCase.actual_rust_category
        actual_cpp_category = $null
        expected_cpp_category = $expectedCppById[$oracleCase.id]
        paired = $false
        paired_match = $false
        not_computed_reason = $null
    }

    if ($oracleCase.status -eq 'computed') {
        $computed++
        if ($cppById.ContainsKey($oracleCase.id)) {
            $cppEntry = $cppById[$oracleCase.id]
            $row.actual_cpp_category = $cppEntry.actual_cpp_category
            $row.paired = ($cppEntry.rust_result_source -eq 'computed_by_rust_oracle')
            $row.paired_match = ($row.actual_cpp_category -eq $row.actual_rust_category)
            if ($row.paired -and -not $row.paired_match) {
                $pairedMismatches++
                $pairedComputedMismatchIds += $oracleCase.id
            }
        }
    }
    else {
        $notComputed++
        $row.not_computed_reason = $oracleCase.detail
        $notComputedIds += $oracleCase.id
    }

    $rows += [pscustomobject]$row
}

$report = [ordered]@{
    schema = 'nrd-paired-differential/v1'
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    tests_exe_path = $TestsExe
    tests_exe_sha256 = $testsSha
    tests_run_id = $RunId
    tests_run_exit = $runExit
    oracle_json = $OracleJson
    oracle_sha256 = (Get-FileHash -LiteralPath $OracleJson -Algorithm SHA256).Hash
    cases_total = $oracle.cases_total
    computed_cases = $computed
    not_computed_cases = $notComputed
    paired_cases = ($rows | Where-Object { $_.paired }).Count
    paired_mismatches = $pairedMismatches
    paired_mismatch_ids = $pairedComputedMismatchIds
    not_computed_ids = $notComputedIds
    claim = 'Paired parity is claimed ONLY for computed cases with paired=true. not_computed cases are listed with reasons and are not evidence of parity.'
    rows = $rows
}

$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $pairedReportPath -Encoding utf8

Write-Host ""
Write-Host "paired report : $pairedReportPath"
Write-Host ("cases total   : {0}" -f $oracle.cases_total)
Write-Host ("computed      : {0}" -f $computed)
Write-Host ("not_computed  : {0} ({1})" -f $notComputed, ($notComputedIds -join ', '))
Write-Host ("paired cases  : {0}" -f ($rows | Where-Object { $_.paired }).Count)
Write-Host ("paired mismatch: {0}" -f $pairedMismatches)
Write-Host ("tests exit    : {0}" -f $runExit)

if ($pairedMismatches -ne 0) {
    Write-Error "Paired Rust/C++ mismatches: $($pairedComputedMismatchIds -join ', ')"
}

exit $runExit
