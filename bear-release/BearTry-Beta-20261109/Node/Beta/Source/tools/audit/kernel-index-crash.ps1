param(
    [Parameter(Mandatory=$true)][string]$TestsExe,
    [Parameter(Mandatory=$true)][string]$ExpectedSha,
    [Parameter(Mandatory=$true)][string]$RunRoot
)
$ErrorActionPreference = 'Stop'
$sha = (Get-FileHash -LiteralPath $TestsExe -Algorithm SHA256).Hash
if ($sha -ne $ExpectedSha) { throw 'Tests.exe SHA mismatch' }
$resolvedExe = (Resolve-Path -LiteralPath $TestsExe).Path
foreach ($point in @('entry','state')) {
    $run = Join-Path $RunRoot $point
    if (Test-Path -LiteralPath $run) { throw "Run directory already exists: $run" }
    $testHome = Join-Path $run 'home'
    $testTemp = Join-Path $run 'temp'
    $out = Join-Path $run 'out'
    New-Item -ItemType Directory -Force -Path $testHome,$testTemp,$out | Out-Null
    $marker = Join-Path $run 'staged.txt'
    $backup = Join-Path $run 'backup'
    $env:HOMEDRIVE = 'C:'
    $env:HOMEPATH = $testHome.Substring(2)
    $env:USERPROFILE = $testHome
    $env:TEMP = $testTemp
    $env:TMP = $testTemp
    $env:GPLUS_KERNEL_CRASH_MARKER = $marker
    $env:GPLUS_KERNEL_CRASH_POINT = $point
    $env:GPLUS_KERNEL_CRASH_BACKUP = $backup
    $child = $null
    try {
        $child = Start-Process -FilePath $resolvedExe -ArgumentList @('KernelIndexCrashChild','--reporter','compact') -WorkingDirectory (Split-Path -Parent $PSScriptRoot) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $out 'child.stdout.log') -RedirectStandardError (Join-Path $out 'child.stderr.log') -PassThru
        $deadline = [DateTime]::UtcNow.AddSeconds(180)
        while (!(Test-Path -LiteralPath $marker)) {
            if ($child.HasExited) { throw "Child exited before reaching $point (exit $($child.ExitCode))" }
            if ([DateTime]::UtcNow -gt $deadline) { throw "Timed out waiting for $point" }
            Start-Sleep -Milliseconds 100
        }
        $lines = @(Get-Content -LiteralPath $marker)
        if ($lines.Count -ne 15 -or $lines[6] -ne $point -or [int]$lines[7] -lt 1) { throw "Invalid marker for $point" }
        if ($child.HasExited) { throw "Child exited instead of pausing at $point" }
        $childPid = $child.Id
        Stop-Process -Id $childPid -Force
        $child.WaitForExit()
        $data = Join-Path $testHome '.GrinPP\AUTOMATED_TESTING'
        if (!(Test-Path -LiteralPath $data)) { throw "Test data missing: $data" }
        Copy-Item -LiteralPath $data -Destination $backup -Recurse -Force
        $recover = Start-Process -FilePath $resolvedExe -ArgumentList @('KernelIndexCrashRecovery','--reporter','compact') -WorkingDirectory (Split-Path -Parent $PSScriptRoot) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $out 'recovery.stdout.log') -RedirectStandardError (Join-Path $out 'recovery.stderr.log') -PassThru
        if (!$recover.WaitForExit(180000)) { Stop-Process -Id $recover.Id -Force; throw "Recovery timeout at $point" }
        $recoveryText = Get-Content -LiteralPath (Join-Path $out 'recovery.stdout.log') -Raw
        $passed = ($recover.ExitCode -eq 0) -and ($recoveryText -match 'Passed 1 test case with')
        [ordered]@{tests_sha256=$sha;point=$point;child_pid=$childPid;child_killed=$true;staged_entries=[int]$lines[7];generation=$lines[0];old_tip=$lines[1];new_tip=$lines[4];recovery_exit=$recover.ExitCode;recovery_passed=$passed} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'manifest.json') -Encoding utf8
        if (!$passed) { throw "Recovery failed at $point; see $out" }
        Write-Host "PASS point=$point child_pid=$childPid staged_entries=$($lines[7])"
    } finally {
        if ($null -ne $child -and !$child.HasExited) { Stop-Process -Id $child.Id -Force; $child.WaitForExit() }
    }
}
