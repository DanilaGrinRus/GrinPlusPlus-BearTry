# Full-project MSVC AddressSanitizer build in a COPIED isolated tree (Line 3).
#
# Resolves the LNK2038 problem of the mixed harness: instead of linking ASan
# objects against the ordinary /MT non-ASan project static libraries, the whole
# project is compiled with /fsanitize=address in a scratch copy of the source
# tree, so every project TU (and every STL annotation pragma) is consistent.
#
# This script NEVER touches:
#   - the real repo tree (it only reads it to create the copy),
#   - the shared build directory C:\DHW\Gplus\.toolchain\grinpp-build,
#   - the repo bin/ output directory (outputs stay inside the copied tree).
#
# Ordinary /W4 /WX build is untouched: the project CMakeLists forces /W4 /WX and
# /MT; we only add /fsanitize=address /Zi (Zi is required to avoid C5072 under
# /WX) and /DEBUG at link time.
#
# Usage:
#   pwsh tools/fuzz/build_full_asan.ps1                     # prepare + configure + build
#   pwsh tools/fuzz/build_full_asan.ps1 -ConfigureOnly
#   pwsh tools/fuzz/build_full_asan.ps1 -BuildOnly          # reuse configured dir
#   pwsh tools/fuzz/build_full_asan.ps1 -FreshTree          # recreate the copy
#
# Targets default: Common;Core;Crypto;PMMR (no Database => no uninstrumented
# rocksdb.lib in the link). Add BlockChain/Net/Tests only in a tree where the
# remaining C++ sysroot libraries are ASan-rebuilt as well.

param(
    [string]$Repo = 'C:\DHW\Gplus\src\grinplusplus',
    [string]$Tree = 'C:\DHW\.tmp\asan-line3\gpp-tree',
    [string]$BuildDir = 'C:\DHW\.tmp\asan-line3\gpp-build',
    [string]$Sysroot = 'C:\DHW\Gplus\.toolchain\sysroot',
    [string]$EvidenceRoot = 'C:\DHW\Gplus\src\grinplusplus\evidence\line3',
    [string[]]$Targets = @('Common', 'Core', 'Crypto', 'PMMR'),
    [int]$TimeoutMin = 90,
    [switch]$FreshTree,
    [switch]$ConfigureOnly,
    [switch]$BuildOnly,
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
$runId = 'full-asan-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$startedUtc = (Get-Date).ToUniversalTime()

# ---------------- safety ----------------
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$TreeFull = [System.IO.Path]::GetFullPath($Tree)
$BuildFull = [System.IO.Path]::GetFullPath($BuildDir)
$scratchRoot = [System.IO.Path]::GetFullPath('C:\DHW\.tmp').TrimEnd('\') + '\'
foreach ($p in @($TreeFull, $BuildFull)) {
    if (-not $p.StartsWith($scratchRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to write outside C:\DHW\.tmp: $p"
    }
    if ($p.StartsWith($Repo, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to write inside the real repo: $p"
    }
}
$sharedBuild = [System.IO.Path]::GetFullPath('C:\DHW\Gplus\.toolchain\grinpp-build')
if ($BuildFull.StartsWith($sharedBuild, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "refusing to use the shared build directory: $BuildFull"
}

$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
foreach ($tool in @($cmake, $msbuild, $vcvars, (Join-Path $Sysroot 'share\fmt\fmt-config.cmake'))) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "required tool/dependency not found: $tool" }
}

function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

# ---------------- source tree copy ----------------
if ($FreshTree -and (Test-Path -LiteralPath $TreeFull)) {
    if (-not $TreeFull.StartsWith($scratchRoot, [System.StringComparison]::OrdinalIgnoreCase)) { throw "unsafe tree removal: $TreeFull" }
    Remove-Item -LiteralPath $TreeFull -Recurse -Force
}
if (-not (Test-Path -LiteralPath $TreeFull)) {
    New-Item -ItemType Directory -Force -Path $TreeFull | Out-Null
    # Copy sources/deps/cmake/tests/tools; exclude binary outputs, VCS and packages.
    $robolog = Join-Path (Split-Path $TreeFull -Parent) "$runId.robocopy.log"
    $rc = & robocopy $Repo $TreeFull /E /XD .git bin dist evidence vcpkg /NFL /NDL /NJH /NJS /R:1 /W:1 2>&1
    $robocode = $LASTEXITCODE
    $rc | Set-Content -LiteralPath $robolog -Encoding utf8
    if ($robocode -ge 8) { throw "robocopy failed with exit code $robocode (see $robolog)" }
    Write-Host "tree copy: $TreeFull (robocopy exit $robocode)"
} else {
    Write-Host "tree copy: reusing $TreeFull"
}

# Source manifest of the copied production tree (exact inputs of the build).
$manifestPairs = New-Object System.Collections.Generic.List[string]
foreach ($rel in @('CMakeLists.txt', 'cmake', 'include', 'src', 'deps', 'tests')) {
    $root = Join-Path $TreeFull $rel
    if (Test-Path -LiteralPath $root -PathType Leaf) {
        $manifestPairs.Add("$rel|$((Get-Item $root).Length)|$(Get-Sha $root)")
    } elseif (Test-Path -LiteralPath $root) {
        Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | ForEach-Object {
            $relPath = $_.FullName.Substring($TreeFull.Length).TrimStart('\')
            $manifestPairs.Add("$relPath|$($_.Length)|$(Get-Sha $_.FullName)")
        }
    }
}
$treeManifestText = ($manifestPairs | Sort-Object) -join "`n"
$treeManifestPath = Join-Path $BuildFull 'source_tree_manifest.txt'
New-Item -ItemType Directory -Force -Path $BuildFull | Out-Null
Set-Content -LiteralPath $treeManifestPath -Value $treeManifestText -Encoding utf8
$treeManifestSha = Get-Sha $treeManifestPath
Write-Host "source manifest: $TreeFull ($($manifestPairs.Count) files, sha $($treeManifestSha.Substring(0,16))...)"

# ---------------- configure ----------------
# MSVC defaults must be preserved: setting CMAKE_CXX_FLAGS replaces the CMake
# default ("/DWIN32 /D_WINDOWS /W3 /GR /EHsc"), and third-party subprojects
# (deps/reproc++) rely on /EHsc. The project CMakeLists replaces /W3 with
# /W4 /WX afterwards. /Zi is required to avoid C5072 under /WX.
$cxxFlags = '/DWIN32 /D_WINDOWS /W3 /GR /EHsc /fsanitize=address /Zi'
$cFlags = '/DWIN32 /D_WINDOWS /W3 /fsanitize=address /Zi'
$configureLog = Join-Path $BuildFull "$runId.configure.log"
$configureArgs = @(
    '-S', $TreeFull, '-B', $BuildFull,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_PREFIX_PATH=$Sysroot",
    '-DGRINPP_TESTS=ON', '-DGRINPP_TOOLS=OFF',
    "-DCMAKE_CXX_FLAGS=$cxxFlags",
    "-DCMAKE_C_FLAGS=$cFlags",
    '-DCMAKE_EXE_LINKER_FLAGS=/DEBUG',
    '-DCMAKE_SHARED_LINKER_FLAGS=/DEBUG',
    '-DCMAKE_STATIC_LINKER_FLAGS=/DEBUG'
)
if ($Reconfigure) { $configureArgs += '--fresh' }
if (-not $BuildOnly) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $cfgOut = & $cmake @configureArgs 2>&1
    $cfgCode = $LASTEXITCODE
    $sw.Stop()
    $cfgOut | Set-Content -LiteralPath $configureLog -Encoding utf8
    if ($cfgCode -ne 0) {
        Write-Host "CONFIGURE FAILED (exit $cfgCode), log: $configureLog"
        $cfgOut | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        exit 1
    }
    Write-Host ("configure OK ({0:N1}s), log: {1}" -f $sw.Elapsed.TotalSeconds, $configureLog)
}
if ($ConfigureOnly) { exit 0 }

# ---------------- build ----------------
$buildLog = Join-Path $BuildFull "$runId.build.log"
$sln = Join-Path $BuildFull 'GRIN_PLUS_PLUS.sln'
if (-not (Test-Path -LiteralPath $sln)) { throw "solution not found (configure first): $sln" }
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$targetSpec = ($Targets -join ';')
$buildArgs = @(
    $sln, "/t:$targetSpec", '/p:Configuration=Release', '/p:Platform=x64',
    '/p:TrackFileAccess=false', '/p:UseSharedCompilation=false', '/nr:false',
    '/v:m', '/nologo', "/flp:LogFile=$buildLog;Verbosity=normal"
)
$buildOut = & $msbuild @buildArgs 2>&1
$buildCode = $LASTEXITCODE
$sw.Stop()
$buildOut | Set-Content -LiteralPath (Join-Path $BuildFull "$runId.msbuild.stdout.log") -Encoding utf8
Write-Host ("build: targets=$targetSpec exit=$buildCode wall={0:N1}s log={1}" -f $sw.Elapsed.TotalSeconds, $buildLog)
if ($buildCode -ne 0) {
    Write-Host 'BUILD FAILED; last output lines:'
    $buildOut | Select-Object -Last 60 | ForEach-Object { Write-Host $_ }
}

# ---------------- outputs / manifest ----------------
$libCandidates = @(
    (Join-Path $TreeFull 'bin\Release\Release'),
    (Join-Path $TreeFull 'bin\Release')
)
$libDir = $libCandidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'Common.lib') } | Select-Object -First 1
$libs = @()
if ($libDir) {
    $libs = @(Get-ChildItem -LiteralPath $libDir -File | Where-Object { $_.Extension -in '.lib', '.exe', '.dll' } | Sort-Object Name | ForEach-Object {
            [ordered]@{ name = $_.Name; bytes = $_.Length; sha256 = (Get-Sha $_.FullName) }
        })
}
$asanDll = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC' -Recurse -Filter 'clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1

$gitHead = (& git -C $Repo rev-parse HEAD).Trim()
$gitStatus = @(& git -C $Repo status --porcelain)
$manifest = [ordered]@{
    schema              = 'gpp-line3-full-asan-build/v1'
    run_id              = $runId
    created_utc         = $startedUtc.ToString('o')
    repo                = $Repo
    git_head            = $gitHead
    git_status          = $gitStatus
    copied_tree         = $TreeFull
    source_manifest     = $treeManifestPath
    source_manifest_sha256 = $treeManifestSha
    build_dir           = $BuildFull
    sysroot             = $Sysroot
    toolchain           = 'MSVC 14.44.35207 (VS 2022 BuildTools 17.14), Windows SDK 10.0.26100'
    asan_runtime        = if ($asanDll) { [ordered]@{ path = $asanDll.FullName; sha256 = (Get-Sha $asanDll.FullName) } } else { $null }
    configure = [ordered]@{ command = ($cmake + ' ' + ($configureArgs -join ' ')); log = $configureLog; exit_code = if ($BuildOnly) { 'reused' } else { $cfgCode } }
    build = [ordered]@{
        command      = ($msbuild + ' ' + ($buildArgs -join ' '))
        targets      = $Targets
        log          = $buildLog
        stdout_log   = (Join-Path $BuildFull "$runId.msbuild.stdout.log")
        exit_code    = $buildCode
        wall_sec     = [Math]::Round($sw.Elapsed.TotalSeconds, 3)
        output_dir   = $libDir
        libraries    = $libs
    }
    notes = @(
        'Project CMakeLists forces /W4 /WX and static CRT /MT; the ASan profile only adds /fsanitize=address /Zi and /DEBUG.',
        '/Zi is mandatory: without debug info MSVC raises C5072 and /WX fails the build.',
        'All project TUs in this build are ASan-instrumented, so MSVC STL annotate_string/annotate_vector pragmas match (LNK2038 resolved).',
        'Uninstrumented: sysroot C libraries (zlib, minizip, libsodium, secp256k1-zkp, roaring) and system CRT/Windows DLLs.',
        'Targets avoid Database so the non-ASan rocksdb.lib C++ library is not linked.'
    )
    result = if ($buildCode -eq 0) { 'BUILD_PASS' } else { 'BUILD_FAIL' }
}

$evDir = Join-Path $EvidenceRoot $runId
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
Copy-Item -LiteralPath $configureLog -Destination $evDir -Force -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath $buildLog) { Copy-Item -LiteralPath $buildLog -Destination $evDir -Force }
Copy-Item -LiteralPath $treeManifestPath -Destination $evDir -Force -ErrorAction SilentlyContinue
$manifestPath = Join-Path $BuildFull 'build_manifest.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $evDir 'build_manifest.json') -Force

Write-Host "manifest: $manifestPath"
Write-Host "evidence: $evDir"
if ($buildCode -eq 0) {
    Write-Host ("BUILD PASS: " + (($libs | ForEach-Object { $_.name }) -join ', '))
    exit 0
}
exit 1
