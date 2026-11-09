# Build the standalone Grin++ ASan fuzz harness (Line 3).
#
# Independent of the normal build: own output directory, own object files.
# It never configures or builds the shared .toolchain/grinpp-build tree and it
# does not modify the project's CMake files or /W4 /WX flags.
#
# Profile (MSVC 14.44 / VS 2022 BuildTools):
#   /W4 /WX /O1 /Zi /MT /EHsc /std:c++17 /fsanitize=address
# Notes:
#   * /Zi is required: MSVC emits warning C5072 when ASan is enabled without
#     debug info, and /WX would otherwise fail the build. /Zi does not weaken
#     the production /W4 /WX flags.
#   * ASan links a dynamic runtime DLL (clang_rt.asan_dynamic-x86_64.dll); the
#     script copies it next to the harness executables.
#   * Project static libraries from the ordinary Release build are linked
#     uninstrumented; only harness/target translation units (and ZipFile.cpp for
#     the zip target) are instrumented. See docs/modernization/SANITIZERS.md.
#
# Usage:
#   pwsh tools/fuzz/build_asan.ps1                 # build all targets
#   pwsh tools/fuzz/build_asan.ps1 -Only zip       # build one target
#   pwsh tools/fuzz/build_asan.ps1 -Clean

param(
    [string]$Repo = 'C:\DHW\Gplus\src\grinplusplus',
    [string]$OutDir = 'C:\DHW\.tmp\asan-line3\fuzz',
    [string]$Sysroot = 'C:\DHW\Gplus\.toolchain\sysroot',
    [string[]]$Only = @(),
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
if (-not (Test-Path -LiteralPath $Sysroot)) { throw "sysroot not found: $Sysroot" }

$Repo = (Resolve-Path -LiteralPath $Repo).Path
$OutDirFull = [System.IO.Path]::GetFullPath($OutDir)
# Guard: never build inside the shared normal build directory or the repo.
$forbidden = @('C:\DHW\Gplus\.toolchain\grinpp-build', (Join-Path $Repo 'bin'))
foreach ($f in $forbidden) {
    if ($OutDirFull.StartsWith($f, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to build into shared/repo build output: $OutDirFull"
    }
}
if ($Clean -and (Test-Path -LiteralPath $OutDirFull)) {
    Remove-Item -LiteralPath $OutDirFull -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $OutDirFull, (Join-Path $OutDirFull 'logs') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $OutDirFull 'obj') | Out-Null

$projectLibDir = Join-Path $Repo 'bin\Release\Release'
$asanDll = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC' -Recurse -Filter 'clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
if (-not $asanDll) { throw 'clang_rt.asan_dynamic-x86_64.dll not found in the MSVC installation' }
Copy-Item -LiteralPath $asanDll.FullName -Destination (Join-Path $OutDirFull 'clang_rt.asan_dynamic-x86_64.dll') -Force

$includes = @(
    (Join-Path $Repo 'include'),
    (Join-Path $Repo 'src'),
    (Join-Path $Repo 'deps'),
    (Join-Path $Repo 'deps\ghc'),
    (Join-Path $Repo 'deps\jsoncpp'),
    (Join-Path $Repo 'deps\mio\single_include'),
    (Join-Path $Repo 'deps\stduuid\include'),
    (Join-Path $Repo 'deps\cppcodec'),
    (Join-Path $Repo 'deps\scheduler\include'),
    (Join-Path $Repo 'deps\bitcoin\include'),
    (Join-Path $Repo 'deps\chachapoly'),
    (Join-Path $Repo 'deps\rfc6234'),
    (Join-Path $Sysroot 'include'),
    (Join-Path $Repo 'tests\fuzz')
)

$defines = @(
    'WIN32_LEAN_AND_MEAN', 'NOMINMAX', '_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS',
    'FMT_HEADER_ONLY=1', 'SODIUM_STATIC', 'ssize_t=SSIZE_T', '_CRT_SECURE_NO_WARNINGS',
    # Same project-wide defines as the ordinary CMake build (root CMakeLists.txt,
    # src/Common/CMakeLists.txt). Required when project TUs are compiled here.
    'UNICODE', '_UNICODE', 'SPDLOG_WCHAR_FILENAMES', 'HAS_MEMSET_S'
)

$cflags = @('/nologo', '/W4', '/WX', '/O1', '/Zi', '/MT', '/EHsc', '/std:c++17',
    '/fsanitize=address',
    # MSVC ASan defines _DISABLE_STRING_ANNOTATION/_DISABLE_VECTOR_ANNOTATION,
    # which changes the STL #pragma detect_mismatch value and makes the harness
    # objects incompatible with the ordinary (uninstrumented) project libs.
    # Undefining them restores the ABI guard value of the linked libs; this is
    # required for mixed instrumented/uninstrumented linking and is documented
    # in docs/modernization/SANITIZERS.md.
    '/U_DISABLE_STRING_ANNOTATION', '/U_DISABLE_VECTOR_ANNOTATION')

# Link libraries shared by the targets that pull project model code.
$sysrootLibs = @(
    (Join-Path $Sysroot 'lib\rocksdb.lib'),
    (Join-Path $Sysroot 'lib\secp256k1-zkp.lib'),
    (Join-Path $Sysroot 'lib\libsodium.lib'),
    (Join-Path $Sysroot 'lib\roaring.lib'),
    (Join-Path $Sysroot 'lib\minizip.lib'),
    (Join-Path $Sysroot 'lib\zlibstatic.lib')
)
$fullProjectLibs = @('API', 'BlockChain', 'Common', 'Core', 'Crypto', 'Database', 'Net', 'Tor', 'PMMR', 'PoW', 'TxPool', 'Wallet') |
    ForEach-Object { Join-Path $projectLibDir "$_.lib" }

$targets = [ordered]@{
    'bytebuffer' = @{
        Sources = @((Join-Path $Repo 'tests\fuzz\fuzz_driver.cpp'), (Join-Path $Repo 'tests\fuzz\target_bytebuffer.cpp'))
        Libs    = @()
    }
    'p2p_body'   = @{
        Sources = @((Join-Path $Repo 'tests\fuzz\fuzz_driver.cpp'), (Join-Path $Repo 'tests\fuzz\target_p2p_messages.cpp'),
                     (Join-Path $Repo 'src\Core\Traits\Serializable.cpp'))
        Libs    = @()
    }
    'segment'    = @{
        Sources = @((Join-Path $Repo 'tests\fuzz\fuzz_driver.cpp'), (Join-Path $Repo 'tests\fuzz\target_segment.cpp'),
                     (Join-Path $Repo 'src\Core\Traits\Serializable.cpp'))
        Libs    = @()
    }
    'nrd_rules'  = @{
        # Header-only production NRD rules (include/PMMR/NrdRules.h) from the
        # line-2 handoff. No project static libraries are linked, so the rule
        # functions themselves are ASan-instrumented.
        Sources = @((Join-Path $Repo 'tests\fuzz\fuzz_driver.cpp'), (Join-Path $Repo 'tests\fuzz\target_nrd_rules.cpp'))
        Libs    = @()
    }
    'zip'        = @{
        # ZipFile.cpp needs the Logger/FileUtil implementations. They are compiled
        # from source under ASan instead of linking Common.lib, because Common.lib
        # carries non-ASan MSVC STL annotation pragmas (LNK2038).
        Sources = @((Join-Path $Repo 'tests\fuzz\fuzz_driver.cpp'), (Join-Path $Repo 'tests\fuzz\target_zip.cpp'),
                     (Join-Path $Repo 'src\PMMR\Zip\ZipFile.cpp'),
                     (Join-Path $Repo 'src\Common\Logger.cpp'),
                     (Join-Path $Repo 'src\Common\GrinStr.cpp'),
                     (Join-Path $Repo 'src\Common\Util\FileUtil.cpp'))
        Libs    = @((Join-Path $Sysroot 'lib\minizip.lib'), (Join-Path $Sysroot 'lib\zlibstatic.lib'),
                     'advapi32.lib', 'kernel32.lib', 'user32.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib', 'uuid.lib')
    }
}

$selected = if ($Only.Count -gt 0) {
    @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
} else { $targets.Keys }
$manifest = [ordered]@{
    schema          = 'gpp-line3-fuzz-build/v1'
    created_utc     = (Get-Date).ToUniversalTime().ToString('o')
    repo            = $Repo
    git_head        = (& git -C $Repo rev-parse HEAD).Trim()
    git_status      = @(& git -C $Repo status --porcelain)
    msvc            = '14.44.35207 (VS 2022 BuildTools 17.14, Windows SDK 10.0.26100)'
    profile         = '/W4 /WX /O1 /Zi /MT /EHsc /std:c++17 /fsanitize=address (dynamic ASan runtime DLL)'
    output_dir      = $OutDirFull
    asan_runtime    = $asanDll.FullName
    uninstrumented  = @('project static libs in bin\Release\Release (Release /MT, no ASan)',
                        'sysroot libs: rocksdb, secp256k1-zkp, libsodium, roaring, minizip, zlibstatic')
    targets         = [ordered]@{}
}

foreach ($name in $selected) {
    if (-not $targets.Contains($name)) { throw "unknown target: $name" }
    $t = $targets[$name]
    $logPath = Join-Path $OutDirFull "logs\build_$name.log"
    $exePath = Join-Path $OutDirFull "fuzz_$name.exe"
    $incArgs = $includes | ForEach-Object { "/I`"$_`"" }
    $defArgs = $defines | ForEach-Object { "/D$_" }
    $srcArgs = $t.Sources | ForEach-Object { "`"$_`"" }
    $libArgs = $t.Libs | ForEach-Object { "`"$_`"" }
    $objDir = Join-Path $OutDirFull "obj\$name"
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $compilerArgs = @($cflags + $incArgs + $defArgs + $srcArgs + @("/Fe:$exePath") +
        @('/link', '/DEBUG', '/INCREMENTAL:NO', '/SUBSYSTEM:CONSOLE', "/PDB:$objDir\fuzz_$name.pdb") + $libArgs)
    $cmd = 'call "' + $vcvars + '" >nul 2>&1 && cl.exe ' + ($compilerArgs -join ' ')
    Push-Location $objDir
    try {
        $output = & cmd /c $cmd 2>&1
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    $output | Set-Content -LiteralPath $logPath -Encoding utf8
    $manifest.targets[$name] = [ordered]@{
        exe        = $exePath
        log        = $logPath
        exit_code  = $code
        sources    = $t.Sources
        libraries  = $t.Libs
        binary_sha256 = if (Test-Path -LiteralPath $exePath) { (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash } else { $null }
    }
    if ($code -ne 0) {
        Write-Host "BUILD FAILED: $name (exit $code) - see $logPath"
        $output | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutDirFull 'build_manifest.json') -Encoding utf8
        exit 1
    }
    Write-Host "built fuzz_$name.exe  sha256=$($manifest.targets[$name].binary_sha256)"
}

$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutDirFull 'build_manifest.json') -Encoding utf8
Write-Host "manifest: $(Join-Path $OutDirFull 'build_manifest.json')"
exit 0
