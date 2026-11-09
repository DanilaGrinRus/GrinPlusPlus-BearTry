# Generate the synthetic fuzz corpus for the Line 3 ASan harness.
#
# Deterministic by construction: every file is either a fixed byte pattern or a
# ZIP built from fixed entry names/payloads. No user data, wallets, seeds or
# captures are included. The corpus is safe to keep in the repository.
#
# Usage: pwsh tools/fuzz/gen_corpus.ps1 [-Force]

param(
    [string]$Repo = 'C:\DHW\Gplus\src\grinplusplus',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$corpusRoot = Join-Path $Repo 'tests\fuzz\corpus'

function Write-Bytes([string]$Target, [string]$Name, [byte[]]$Bytes) {
    $dir = Join-Path $corpusRoot $Target
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $path = Join-Path $dir $Name
    if ((Test-Path -LiteralPath $path) -and -not $Force) { return }
    [System.IO.File]::WriteAllBytes($path, $Bytes)
}

function U64([uint64]$v) {
    $b = [BitConverter]::GetBytes($v)
    [array]::Reverse($b)
    return [byte[]]$b
}

function Join-Bytes {
    param([Parameter(ValueFromRemainingArguments = $true)][object[]]$Parts)
    $ms = [System.IO.MemoryStream]::new()
    foreach ($p in $Parts) {
        if ($null -eq $p) { continue }
        $b = [byte[]]$p
        $ms.Write($b, 0, $b.Length)
    }
    return $ms.ToArray()
}

# ---------------- bytebuffer ----------------
Write-Bytes 'bytebuffer' 'empty.bin' ([byte[]]@())
Write-Bytes 'bytebuffer' 'one_byte.bin' ([byte[]]@(0x01))
Write-Bytes 'bytebuffer' 'u64_zero.bin' (Join-Bytes ([byte[]]@(0x0D)) (U64 0))
Write-Bytes 'bytebuffer' 'u64_max.bin' (Join-Bytes ([byte[]]@(0x0D)) (U64 ([uint64]::MaxValue)))
Write-Bytes 'bytebuffer' 'u64_declared_9.bin' (Join-Bytes ([byte[]]@(0x0D)) (U64 9) ([byte[]]@(1,2,3,4,5,6,7,8)))
Write-Bytes 'bytebuffer' 'u64_declared_plus1.bin' (Join-Bytes ([byte[]]@(0x0D)) (U64 9) ([byte[]]@(1,2,3,4,5,6,7,8)))
Write-Bytes 'bytebuffer' 'u64_declared_ffffffff.bin' (Join-Bytes ([byte[]]@(0x0D)) (U64 ([uint64]4294967295)) ([byte[]]@(0,0,0,0)))
Write-Bytes 'bytebuffer' 'truncated_u32.bin' ([byte[]]@(0x04,0x00,0x00,0x00))
Write-Bytes 'bytebuffer' 'truncated_u64.bin' ([byte[]]@(0x07,0x00,0x00,0x00,0x00,0x00,0x00))
Write-Bytes 'bytebuffer' 'bigint32_short.bin' (Join-Bytes ([byte[]]@(0x0B)) ([byte[]]::new(31)))
Write-Bytes 'bytebuffer' 'bigint64_short.bin' (Join-Bytes ([byte[]]@(0x0C)) ([byte[]]::new(63)))
Write-Bytes 'bytebuffer' 'varstr_huge.bin' (Join-Bytes ([byte[]]@(0x09)) (U64 0x7FFFFFFFFFFFFFFF) ([byte[]]@(0x41)))
$ops = [byte[]]::new(64)
for ($i = 0; $i -lt 64; $i++) { $ops[$i] = [byte]($i % 16) }
Write-Bytes 'bytebuffer' 'opcode_stream.bin' $ops

# ---------------- p2p_body ----------------
$hash32 = [byte[]]::new(32); for ($i = 0; $i -lt 32; $i++) { $hash32[$i] = [byte](0xA0 + ($i % 16)) }
Write-Bytes 'p2p_body' 'empty_body.bin' ([byte[]]@(0x00))
Write-Bytes 'p2p_body' 'request_ident.bin' (Join-Bytes ([byte[]]@(0x00)) $hash32 ([byte[]]@(0x09)) (U64 0))
Write-Bytes 'p2p_body' 'request_big_height.bin' (Join-Bytes ([byte[]]@(0x00)) $hash32 ([byte[]]@(0xFF)) (U64 1))
Write-Bytes 'p2p_body' 'request_truncated_hash.bin' (Join-Bytes ([byte[]]@(0x02)) ([byte[]]::new(31)))
Write-Bytes 'p2p_body' 'request_idx_max.bin' (Join-Bytes ([byte[]]@(0x04)) $hash32 ([byte[]]@(0x0D)) (U64 ([uint64]::MaxValue)))
Write-Bytes 'p2p_body' 'response_bitmap.bin' (Join-Bytes ([byte[]]@(0x01)) $hash32 ([byte[]]@(0x09)) (U64 0) ([byte[]]@(0x00,0x01)) ([byte[]]@(0x00,0x01)) $hash32)
Write-Bytes 'p2p_body' 'response_outputs_empty.bin' (Join-Bytes ([byte[]]@(0x03)) $hash32 (U64 0) (U64 0) (U64 0) (U64 0) $hash32)
Write-Bytes 'p2p_body' 'response_kernels_count_max.bin' (Join-Bytes ([byte[]]@(0x07)) $hash32 (U64 ([uint64]::MaxValue)))
Write-Bytes 'p2p_body' 'truncated_hash.bin' (Join-Bytes ([byte[]]@(0x05)) ([byte[]]::new(17)))

# ---------------- segment ----------------
Write-Bytes 'segment' 'empty.bin' ([byte[]]@(0x00))
Write-Bytes 'segment' 'ident_valid.bin' (Join-Bytes ([byte[]]@(0x00,0x09)) (U64 0))
Write-Bytes 'segment' 'ident_height33.bin' (Join-Bytes ([byte[]]@(0x00,0x21)) (U64 0))
Write-Bytes 'segment' 'ident_height32_idxmax.bin' (Join-Bytes ([byte[]]@(0x00,0x20)) (U64 ([uint64]::MaxValue)))
Write-Bytes 'segment' 'proof_empty.bin' (Join-Bytes ([byte[]]@(0x01)) (U64 0))
Write-Bytes 'segment' 'proof_one_hash.bin' (Join-Bytes ([byte[]]@(0x01)) (U64 1) $hash32)
Write-Bytes 'segment' 'proof_count_max.bin' (Join-Bytes ([byte[]]@(0x01)) (U64 ([uint64]::MaxValue)))
Write-Bytes 'segment' 'proof_count_1e6.bin' (Join-Bytes ([byte[]]@(0x01)) (U64 1000000))
Write-Bytes 'segment' 'bitmap_raw_block.bin' (Join-Bytes ([byte[]]@(0x02,0x00,0x00)) ([byte[]]::new(128)))
Write-Bytes 'segment' 'bitmap_block_sparse.bin' (Join-Bytes ([byte[]]@(0x02,0x00,0x01,0x02,0x00,0x03,0x00,0x00,0x00,0x09)))
Write-Bytes 'segment' 'chunk.bin' (Join-Bytes ([byte[]]@(0x03)) ([byte[]]::new(128)))
Write-Bytes 'segment' 'bitmap_segment.bin' (Join-Bytes ([byte[]]@(0x04,0x09)) (U64 0) ([byte[]]@(0x00,0x01,0x00,0x00)) ([byte[]]::new(128)) (U64 0))
Write-Bytes 'segment' 'segment_bitmapchunk.bin' (Join-Bytes ([byte[]]@(0x05,0x09)) (U64 0) (U64 0) (U64 0) (U64 0) (U64 0))
Write-Bytes 'segment' 'segment_proof_validate.bin' (Join-Bytes ([byte[]]@(0x06)) (U64 1) $hash32)

# ---------------- zip ----------------
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem | Out-Null
function New-Zip([hashtable]$Entries) {
    $ms = [System.IO.MemoryStream]::new()
    $zip = [System.IO.Compression.ZipArchive]::new($ms, [System.IO.Compression.ZipArchiveMode]::Create, $true)
    foreach ($k in ($Entries.Keys | Sort-Object)) {
        $entry = $zip.CreateEntry($k, [System.IO.Compression.CompressionLevel]::NoCompression)
        $s = $entry.Open()
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($Entries[$k])
        $s.Write($bytes, 0, $bytes.Length)
        $s.Dispose()
    }
    $zip.Dispose()
    return $ms.ToArray()
}

$validZip = New-Zip ([ordered]@{ 'CHAIN/candidate.chain' = 'synthetic-candidate-chain-payload'; 'CHAIN/kernel' = 'synthetic-kernel-payload' })
Write-Bytes 'zip' 'valid.zip' $validZip
Write-Bytes 'zip' 'empty.bin' ([byte[]]@())
Write-Bytes 'zip' 'random.bin' ([byte[]](1..64 | ForEach-Object { [byte](($_ * 37) % 251) }))
Write-Bytes 'zip' 'traversal.zip' (New-Zip ([ordered]@{ '../escape.txt' = 'escape-attempt'; 'ok.txt' = 'ok' }))
Write-Bytes 'zip' 'duplicate_names.zip' (New-Zip ([ordered]@{ 'same' = 'first-copy'; 'same2' = 'second-copy' }))
$crc = [byte[]]$validZip.Clone()
$needle = [System.Text.Encoding]::ASCII.GetBytes('synthetic-candidate-chain-payload')
for ($i = 0; $i -le $crc.Length - $needle.Length; $i++) {
    $match = $true
    for ($j = 0; $j -lt $needle.Length; $j++) { if ($crc[$i + $j] -ne $needle[$j]) { $match = $false; break } }
    if ($match) { $crc[$i] = $crc[$i] -bxor 0x01; break }
}
Write-Bytes 'zip' 'crc_corrupted.zip' $crc
$truncated = [byte[]]::new($validZip.Length - 48)
[array]::Copy($validZip, 0, $truncated, 0, $truncated.Length)
Write-Bytes 'zip' 'truncated_central_directory.zip' $truncated

# ---------------- nrd_rules (line-2 handoff) ----------------
# Seeds are derived from the machine-readable NRD differential cases
# (tools/audit/nrd/cases/nrd_cases.json) plus explicit boundary values from the
# line-2 handoff: relative_height 0 / WEEK_HEIGHT+1 / 0xFFFF, currentHeight below
# previousHeight, cutoff newer than previousHeight and duplicate excess inside a
# block. Layout matches tests/fuzz/target_nrd_rules.cpp:
#   [0]=selector, [1..8]=prev, [9..16]=current, [17..24]=relative,
#   [25..32]=cutoff, [33..]=33-byte commitments (big-endian value in last 8 bytes)
function Nrd-Commitment([uint64]$Value) {
    $b = [byte[]]::new(33)
    $v = U64 $Value
    [array]::Copy($v, 0, $b, 25, 8)
    return $b
}

function Nrd-Seed([byte]$Selector, [uint64]$Prev, [uint64]$Current, [uint64]$Relative, [uint64]$Cutoff, [byte[]]$Excess = [byte[]]@()) {
    return (Join-Bytes ([byte[]]@($Selector)) (U64 $Prev) (U64 $Current) (U64 $Relative) (U64 $Cutoff) $Excess)
}

$weekHeight = [uint64](2 * 7 * 24 * 60)
Write-Bytes 'nrd_rules' 'boundary_rel_0.bin' (Nrd-Seed 2 100 200 0 0)
Write-Bytes 'nrd_rules' 'boundary_rel_1.bin' (Nrd-Seed 2 100 200 1 0)
Write-Bytes 'nrd_rules' 'boundary_rel_week.bin' (Nrd-Seed 2 100 20100 $weekHeight 0)
Write-Bytes 'nrd_rules' 'boundary_rel_week_plus_1.bin' (Nrd-Seed 2 100 20100 ($weekHeight + 1) 0)
Write-Bytes 'nrd_rules' 'boundary_rel_ffff.bin' (Nrd-Seed 2 100 200 0xFFFF 0)
Write-Bytes 'nrd_rules' 'boundary_rel_max.bin' (Nrd-Seed 2 100 200 ([uint64]::MaxValue) 0)
Write-Bytes 'nrd_rules' 'underflow_current_below_prev.bin' (Nrd-Seed 0 10 5 1 0)
Write-Bytes 'nrd_rules' 'cutoff_newer_than_prev.bin' (Nrd-Seed 0 5 5 1000000 100)
Write-Bytes 'nrd_rules' 'prev_max_current_zero.bin' (Nrd-Seed 0 ([uint64]::MaxValue) 0 1 0)
$same = Nrd-Commitment 7
Write-Bytes 'nrd_rules' 'duplicate_excess_same.bin' (Nrd-Seed 1 1 2 1 0 (Join-Bytes $same $same $same $same))
Write-Bytes 'nrd_rules' 'duplicate_excess_mixed.bin' (Nrd-Seed 1 1 2 1 0 (Join-Bytes (Nrd-Commitment 1) (Nrd-Commitment 2) (Nrd-Commitment 1) (Nrd-Commitment 3) (Nrd-Commitment 2)))
Write-Bytes 'nrd_rules' 'combined_sweep_and_duplicates.bin' (Nrd-Seed 3 0 1 2 0 (Join-Bytes (Nrd-Commitment 42) (Nrd-Commitment 42)))

$nrdCasesPath = Join-Path $Repo 'tools\audit\nrd\cases\nrd_cases.json'
if (Test-Path -LiteralPath $nrdCasesPath) {
    $nrdCases = Get-Content -LiteralPath $nrdCasesPath -Raw | ConvertFrom-Json
    foreach ($case in $nrdCases.cases) {
        $rel = [uint64]0
        $excessParts = @()
        foreach ($kernel in @($case.target)) {
            if ($null -ne $kernel.relative_height) { $rel = [uint64]$kernel.relative_height }
            if ($null -ne $kernel.excess) { $excessParts += (Nrd-Commitment ([uint64]$kernel.excess)) }
        }
        $prev = [uint64](@($case.blocks).Count)
        $safeId = ($case.id -replace '[^A-Za-z0-9._-]', '_')
        $seed = Nrd-Seed 3 $prev ($prev + 1) $rel 0 (Join-Bytes @excessParts)
        Write-Bytes 'nrd_rules' ("case_" + $safeId + ".bin") $seed
    }
    Write-Host ("  nrd_rules: derived {0} seeds from {1}" -f @($nrdCases.cases).Count, $nrdCasesPath)
} else {
    Write-Host "  nrd_rules: case file not found, only boundary seeds generated"
}

# ---------------- p2p_responses / segment_validate (full-ASan linked targets) ----------------
# Reuse the parse-only corpus bodies, re-prefixed with the linked targets'
# selector byte so the same wire shapes reach the instrumented deserializer and
# validation paths.
$bodySources = Join-Path $corpusRoot 'p2p_body'
if (Test-Path -LiteralPath $bodySources) {
    foreach ($f in Get-ChildItem -LiteralPath $bodySources -File | Sort-Object Name) {
        $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
        $body = if ($bytes.Length -gt 1) { [byte[]]$bytes[1..($bytes.Length - 1)] } else { [byte[]]@() }
        for ($sel = 0; $sel -lt 4; $sel++) {
            Write-Bytes 'p2p_responses' ("sel{0}_{1}" -f $sel, $f.Name) (Join-Bytes ([byte[]]@($sel)) $body)
        }
    }
}
$segmentSources = Join-Path $corpusRoot 'segment'
if (Test-Path -LiteralPath $segmentSources) {
    foreach ($f in Get-ChildItem -LiteralPath $segmentSources -File | Sort-Object Name) {
        $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
        $body = if ($bytes.Length -gt 1) { [byte[]]$bytes[1..($bytes.Length - 1)] } else { [byte[]]@() }
        for ($sel = 0; $sel -lt 6; $sel++) {
            Write-Bytes 'segment_validate' ("op{0}_{1}" -f $sel, $f.Name) (Join-Bytes ([byte[]]@($sel)) $body)
        }
    }
}

Write-Host "corpus written under $corpusRoot"
Get-ChildItem $corpusRoot -Recurse -File | Group-Object { $_.Directory.Name } | ForEach-Object {
    Write-Host ("  {0}: {1} files, {2} bytes" -f $_.Name, $_.Count, (($_.Group | Measure-Object Length -Sum).Sum))
}
