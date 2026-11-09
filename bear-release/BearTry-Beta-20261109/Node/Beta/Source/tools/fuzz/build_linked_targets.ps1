# Build ASan harness targets LINKED against the full-project ASan libraries
# produced by tools/fuzz/build_full_asan.ps1 (copied isolated tree).
#
# This resolves the LNK2038 limitation: every project TU in the copied tree is
# compiled with /fsanitize=address, so the MSVC STL annotate_string/annotate_vector
# pragmas match the harness objects. Database/rocksdb are not linked.
#
# Targets:
#   p2p_responses     SegmentResponseMessage 22/24/26/28 deserialize + bounded
#                     Root/Validate (instrumented Core/Crypto/PMMR)
#   segment_validate  Segment<T>/SegmentProof Root/Validate/ReconstructRoot
#   tests_mini        Catch2 exe with selected production-path tests:
#                     Test_NrdDifferential, Test_NrdIndexRebuild, Test_Segment,
#                     Test_P2PCompat + BlockValidator.cpp, run under ASan
#
# Usage:
#   pwsh tools/fuzz/build_linked_targets.ps1
#   pwsh tools/fuzz/build_linked_targets.ps1 -Only p2p_responses

param(
    [string]$Repo = 'C:\DHW\Gplus\src\grinplusplus',
    [string]$Tree = 'C:\DHW\.tmp\asan-line3\gpp-tree',
    [string]$LibDir = 'C:\DHW\.tmp\asan-line3\gpp-tree\bin\Release\Release',
    [string]$Sysroot = 'C:\DHW\Gplus\.toolchain\sysroot',
    [string]$OutDir = 'C:\DHW\.tmp\asan-line3\fuzz-linked',
    [string]$EvidenceRoot = 'C:\DHW\Gplus\src\grinplusplus\evidence\line3',
    [string[]]$Only = @(),
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$Tree = (Resolve-Path -LiteralPath $Tree).Path
$LibDir = (Resolve-Path -LiteralPath $LibDir).Path
$OutFull = [System.IO.Path]::GetFullPath($OutDir)
if (-not $OutFull.StartsWith([System.IO.Path]::GetFullPath('C:\DHW\.tmp').TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "refusing to write outside C:\DHW\.tmp: $OutFull"
}
if ($Clean -and (Test-Path -LiteralPath $OutFull)) { Remove-Item -LiteralPath $OutFull -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutFull, (Join-Path $OutFull 'logs'), (Join-Path $OutFull 'obj') | Out-Null

$asanDll = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC' -Recurse -Filter 'clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
if (-not $asanDll) { throw 'ASan runtime DLL not found' }
Copy-Item -LiteralPath $asanDll.FullName -Destination (Join-Path $OutFull 'clang_rt.asan_dynamic-x86_64.dll') -Force

$includes = @(
    (Join-Path $Tree 'include'),
    (Join-Path $Tree 'src'),
    (Join-Path $Tree 'deps'),
    (Join-Path $Tree 'deps\ghc'),
    (Join-Path $Tree 'deps\jsoncpp'),
    (Join-Path $Tree 'deps\mio\single_include'),
    (Join-Path $Tree 'deps\stduuid\include'),
    (Join-Path $Tree 'deps\cppcodec'),
    (Join-Path $Tree 'deps\scheduler\include'),
    (Join-Path $Tree 'deps\bitcoin\include'),
    (Join-Path $Tree 'deps\chachapoly'),
    (Join-Path $Tree 'deps\rfc6234'),
    (Join-Path $Tree 'tests\framework\include'),
    (Join-Path $Repo 'tests\fuzz'),
    (Join-Path $Sysroot 'include'),
    (Join-Path $Sysroot 'include\catch2')
)
$defines = @(
    'WIN32_LEAN_AND_MEAN', 'NOMINMAX', 'UNICODE', '_UNICODE', 'SPDLOG_WCHAR_FILENAMES',
    'HAS_MEMSET_S', 'FMT_HEADER_ONLY=1', 'SODIUM_STATIC', 'ssize_t=SSIZE_T',
    '_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS', '_CRT_SECURE_NO_WARNINGS', 'INCLUDE_TEST_MATH'
)
$cflags = @('/nologo', '/W4', '/WX', '/O1', '/Zi', '/MT', '/EHsc', '/std:c++17', '/fsanitize=address')

$projectLibs = @(Get-ChildItem -LiteralPath $LibDir -Filter '*.lib' -File | ForEach-Object { $_.FullName })
$sysrootLibs = @(
    (Join-Path $Sysroot 'lib\zlibstatic.lib'),
    (Join-Path $Sysroot 'lib\minizip.lib'),
    (Join-Path $Sysroot 'lib\libsodium.lib'),
    (Join-Path $Sysroot 'lib\secp256k1-zkp.lib'),
    (Join-Path $Sysroot 'lib\roaring.lib')
)
$systemLibs = @('advapi32.lib', 'kernel32.lib', 'user32.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'ws2_32.lib')
$reprocLibs = @()
foreach ($p in @(
        'C:\DHW\.tmp\asan-line3\gpp-build\deps\reproc\reproc++\lib\Release\reproc++.lib',
        'C:\DHW\.tmp\asan-line3\gpp-build\deps\reproc\reproc\lib\Release\reproc.lib')) {
    if (Test-Path -LiteralPath $p) { $reprocLibs += $p }
}
$linkLibs = @($projectLibs + $sysrootLibs + $reprocLibs + $systemLibs)

$targets = [ordered]@{
    'p2p_responses'    = @{
        Sources = @((Join-Path $Repo 'tests\fuzz\fuzz_driver.cpp'), (Join-Path $Repo 'tests\fuzz\target_p2p_responses.cpp'))
        Libs    = $linkLibs
    }
    'segment_validate' = @{
        Sources = @((Join-Path $Repo 'tests\fuzz\fuzz_driver.cpp'), (Join-Path $Repo 'tests\fuzz\target_segment_validate.cpp'))
        Libs    = $linkLibs
    }
    'tests_mini'       = @{
        Sources = @(
            # Fixed TestMain.cpp (CATCH_CONFIG_RUNNER + local runtime lifetime):
            # the copied tree is synced from the repo before this build, so the
            # ASan regression exercises the actual proposed normal-build source.
            (Join-Path $Tree 'tests\src\TestMain.cpp'),
            (Join-Path $Tree 'tests\src\PMMR\Test_NrdDifferential.cpp'),
            (Join-Path $Tree 'tests\src\PMMR\Test_NrdIndexRebuild.cpp'),
            # Task-5/D2 NRD tests: pure rules, pre-commit gate, mainnet D2 policy
            # (BlockValidator::VerifyNrdNetworkGate / VerifySelfConsistent path).
            (Join-Path $Tree 'tests\src\PMMR\Test_NrdRules.cpp'),
            (Join-Path $Tree 'tests\src\PMMR\Test_NrdPolicy.cpp'),
            (Join-Path $Tree 'tests\src\PMMR\Test_Segment.cpp'),
            # Line-1 archive-server tests (Lead: include in ASan subset when present
            # and link deps permit). Fixed lease fixture (pFirst/pSecond.reset +
            # lease.use_count) is expected to pass; mmap/minizip-based cases may be
            # link/runtime limited.
            (Join-Path $Tree 'tests\src\PMMR\Test_PibdServing.cpp'),
            # A4 closure: F13/archive readiness test needs StateSyncer.cpp in
            # this ASan-only TestsMini link set. Normal /W4 /WX build unaffected.
            (Join-Path $Tree 'src\P2P\Sync\StateSyncer.cpp'),
            # StateSyncer's connection surface; ASan-only link deps.
            (Join-Path $Tree 'src\P2P\ConnectionManager.cpp'),
            (Join-Path $Tree 'src\P2P\Connection.cpp'),
            (Join-Path $Tree 'src\P2P\Sync\Syncer.cpp'),
            (Join-Path $Tree 'src\P2P\Sync\BlockSyncer.cpp'),
            (Join-Path $Tree 'src\P2P\Sync\HeaderSyncer.cpp'),
            (Join-Path $Tree 'src\P2P\Pipeline\BlockPipe.cpp'),
            (Join-Path $Tree 'src\P2P\Pipeline\TransactionPipe.cpp'),
            (Join-Path $Tree 'src\P2P\Pipeline\TxHashSetPipe.cpp'),
            (Join-Path $Tree 'src\P2P\Seed\Seeder.cpp'),
            (Join-Path $Tree 'src\P2P\Seed\PeerManager.cpp'),
            (Join-Path $Tree 'src\P2P\Seed\HandShake.cpp'),
            (Join-Path $Tree 'src\P2P\Seed\DNSSeeder.cpp'),
            (Join-Path $Tree 'src\P2P\P2PServerImpl.cpp'),
            (Join-Path $Tree 'src\P2P\BlockLocator.cpp'),
            (Join-Path $Tree 'src\P2P\MessageProcessor.cpp'),
            (Join-Path $Tree 'src\P2P\Dandelion.cpp'),
            (Join-Path $Tree 'tests\src\Net\Test_P2PCompat.cpp'),
            (Join-Path $Tree 'src\BlockChain\Validators\BlockValidator.cpp'),
            (Join-Path $Repo 'tests\fuzz\stub_chain_getbyheight.cpp')
        )
        Libs    = $linkLibs
    }
    'diag_lifetime'    = @{
        Sources = @((Join-Path $Repo 'tests\fuzz\diag_runtime_lifetime.cpp'))
        Libs    = $linkLibs
    }
    # Negative control: PRE-FIX TestMain (git HEAD version: static TestRuntime)
    # kept in the copied tree as TestMain_legacy_static.cpp. Used only to prove
    # that run_asan_teardown_regression.ps1 fails on the original UAF.
    'tests_mini_prefix' = @{
        Sources = @(
            (Join-Path $Tree 'tests\src\TestMain_legacy_static.cpp'),
            (Join-Path $Tree 'tests\src\PMMR\Test_NrdDifferential.cpp'),
            (Join-Path $Tree 'tests\src\PMMR\Test_NrdIndexRebuild.cpp'),
            (Join-Path $Tree 'tests\src\PMMR\Test_Segment.cpp'),
            (Join-Path $Tree 'tests\src\Net\Test_P2PCompat.cpp'),
            (Join-Path $Tree 'src\BlockChain\Validators\BlockValidator.cpp'),
            (Join-Path $Repo 'tests\fuzz\stub_chain_getbyheight.cpp')
        )
        Libs    = $linkLibs
    }
}

$selected = if ($Only.Count -gt 0) { @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) } else { $targets.Keys }
$manifest = [ordered]@{
    schema        = 'gpp-line3-linked-asan-v1'
    created_utc   = (Get-Date).ToUniversalTime().ToString('o')
    repo          = $Repo
    git_head      = (& git -C $Repo rev-parse HEAD).Trim()
    git_status    = @(& git -C $Repo status --porcelain)
    copied_tree   = $Tree
    lib_dir       = $LibDir
    profile       = '/W4 /WX /O1 /Zi /MT /EHsc /std:c++17 /fsanitize=address, linked against full-project ASan libs'
    asan_runtime  = [ordered]@{ path = $asanDll.FullName; sha256 = (Get-FileHash $asanDll.FullName -Algorithm SHA256).Hash }
    libraries     = @($projectLibs | ForEach-Object { [ordered]@{ name = (Split-Path $_ -Leaf); sha256 = (Get-FileHash $_ -Algorithm SHA256).Hash; bytes = (Get-Item $_).Length } })
    targets       = [ordered]@{}
}

foreach ($name in $selected) {
    if (-not $targets.Contains($name)) { throw "unknown target: $name" }
    $t = $targets[$name]
    $objDir = Join-Path $OutFull "obj\$name"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $exePath = Join-Path $OutFull "fuzz_$name.exe"
    if ($name -eq 'tests_mini') { $exePath = Join-Path $OutFull 'TestsMini.exe' }
    $logPath = Join-Path $OutFull "logs\build_$name.log"

    $incArgs = $includes | ForEach-Object { "/I`"$_`"" }
    $defArgs = $defines | ForEach-Object { "/D$_" }
    $srcArgs = $t.Sources | ForEach-Object { "`"$_`"" }
    $libArgs = $t.Libs | ForEach-Object { "`"$_`"" }
    $compilerArgs = @($cflags + $incArgs + $defArgs + $srcArgs + @("/Fe:$exePath") +
        @('/link', '/DEBUG', '/INCREMENTAL:NO', '/SUBSYSTEM:CONSOLE', "/PDB:$objDir\$name.pdb") + $libArgs)
    $cmd = 'call "' + $vcvars + '" >nul 2>&1 && cd /d "' + $objDir + '" && cl.exe ' + ($compilerArgs -join ' ')
    $output = & cmd /c $cmd 2>&1
    $code = $LASTEXITCODE
    $output | Set-Content -LiteralPath $logPath -Encoding utf8
    $manifest.targets[$name] = [ordered]@{
        exe           = $exePath
        log           = $logPath
        exit_code     = $code
        sources       = $t.Sources
        binary_sha256 = if (Test-Path -LiteralPath $exePath) { (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash } else { $null }
        bytes         = if (Test-Path -LiteralPath $exePath) { (Get-Item $exePath).Length } else { $null }
    }
    if ($code -ne 0) {
        Write-Host "BUILD FAILED: $name (exit $code) - $logPath"
        $output | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        $manifestPath = Join-Path $OutFull 'build_manifest.json'
        $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
        exit 1
    }
    Write-Host ("built {0}  sha256={1}" -f (Split-Path $exePath -Leaf), $manifest.targets[$name].binary_sha256)
}

$manifestPath = Join-Path $OutFull 'build_manifest.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
$evDir = Join-Path $EvidenceRoot ('linked-asan-build-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
Copy-Item -LiteralPath $manifestPath -Destination $evDir -Force
Copy-Item -Path (Join-Path $OutFull 'logs\*') -Destination $evDir -Force -ErrorAction SilentlyContinue
Write-Host "manifest: $manifestPath"
Write-Host "evidence: $evDir"
exit 0
