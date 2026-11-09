param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$SourceProfile,
    [Parameter(Mandatory=$true)][string]$WorkRoot,
    [ValidateSet('Core','Lock')][string]$Case = 'Core'
)

$ErrorActionPreference = 'Stop'

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw "ASSERT: $message" }
}

function Write-Config([string]$path, [string]$dataPath, [int]$rest, [int]$p2p) {
    $json = [ordered]@{
        DATA_PATH = $dataPath
        LOGGER = @{ LOG_LEVEL = 'INFO' }
        P2P = @{
            # Core/Lock do not need peers; disabling them avoids self-connect.
            MAX_PEERS = 0
            MIN_PEERS = 0
            MIN_SYNC_PEERS = 0
            P2P_PORT = $p2p
            P2P_PEER_PORT = $p2p
            PIBD_ENABLED = $false
            PREFERRED_PEERS = @()
            ALLOWED_PEERS = @('127.0.0.1')
        }
        SERVER = @{ REST_API_PORT = $rest }
    }
    $json | ConvertTo-Json -Depth 6 | Set-Content $path -Encoding UTF8
}

function Start-Node([string]$name, [string]$dataPath, [string]$config, [string]$mode, [string]$bootstrap, [string]$anchor) {
    $dir = Join-Path $WorkRoot $name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $env:GRINPP_HEADER_CKPT = $mode
    $env:GRINPP_HEADER_CKPT_BOOTSTRAP = $bootstrap
    $env:GRINPP_HEADER_CKPT_THREADS = '1'
    $env:GRINPP_HEADER_CKPT_SEGMENT = '10000'
    if ($anchor) { $env:GRINPP_HEADER_CKPT_TRUST_ANCHOR = $anchor }
    else { Remove-Item Env:GRINPP_HEADER_CKPT_TRUST_ANCHOR -ErrorAction SilentlyContinue }
    $env:TEMP = Join-Path $dir 'temp'
    $env:TMP = $env:TEMP
    New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null
    $log = Join-Path $dataPath 'LOGS\Node.log'
    Remove-Item $log -Force -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $Exe `
        -ArgumentList @('--floonet','--no-wallet','--headless','--config-path',$config) `
        -PassThru -RedirectStandardOutput (Join-Path $dir 'stdout.txt') `
        -RedirectStandardError (Join-Path $dir 'stderr.txt') -WindowStyle Hidden
    return @{ Process = $p; Log = $log }
}

function Wait-Log($node, [string]$pattern, [int]$timeoutSec = 180) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
        Start-Sleep -Milliseconds 500
        if ($node.Process.HasExited) { break }
        if (Test-Path $node.Log) {
            $text = Get-Content $node.Log -Raw -ErrorAction SilentlyContinue
            if ($text -match $pattern) { return $text }
        }
    }
    $text = if (Test-Path $node.Log) { Get-Content $node.Log -Raw -ErrorAction SilentlyContinue } else { '' }
    return $text
}

function Stop-Node($node) {
    if ($node -and $node.Process -and -not $node.Process.HasExited) {
        Stop-Process -Id $node.Process.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 500
}

if ($SourceProfile -match '(^|[\\/])live-merged([\\/]|$)' -or $SourceProfile -match 'RC[12]') {
    throw "Refusing to use a live/RC profile path: $SourceProfile"
}
if (Test-Path $WorkRoot) { Remove-Item $WorkRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null

$profile = Join-Path $WorkRoot 'profile'
Copy-Item $SourceProfile $profile -Recurse -Force
$config = Join-Path $WorkRoot 'config.json'
Write-Config $config $profile 28913 28914

$summary = [ordered]@{ case = $Case; steps = @(); work = $WorkRoot }

if ($Case -eq 'Core') {
    # 1. Measurement aggregate.
    $n = Start-Node 'hash' $profile $config 'hash' '0' ''
    $text = Wait-Log $n 'event=header_checkpoint_hash_only' 300
    Stop-Node $n
    Assert-True ($text -match 'aggregate=([0-9a-fA-F]{64})') 'hash aggregate missing'
    $anchor = $Matches[1]
    $summary.steps += @{ hash_aggregate = $anchor }

    # 2. Bootstrap without anchor must refuse.
    Remove-Item (Join-Path $profile 'NODE\CHAIN\header.ckpt') -Force -ErrorAction SilentlyContinue
    $n = Start-Node 'noanchor' $profile $config 'checkpoint' '1' ''
    $text = Wait-Log $n 'trust_anchor_required|header_checkpoint_write' 300
    Stop-Node $n
    Assert-True ($text -match 'reason=trust_anchor_required') 'bootstrap without anchor was not refused'
    Assert-True (-not (Test-Path (Join-Path $profile 'NODE\CHAIN\header.ckpt'))) 'checkpoint created without anchor'
    $summary.steps += @{ no_anchor = 'PASS' }

    # 3. Wrong anchor must refuse.
    $n = Start-Node 'badanchor' $profile $config 'checkpoint' '1' 'DEADBEEF'
    $text = Wait-Log $n 'trust_anchor_mismatch|header_checkpoint_write' 300
    Stop-Node $n
    Assert-True ($text -match 'reason=trust_anchor_mismatch') 'wrong anchor was not refused'
    Assert-True (-not (Test-Path (Join-Path $profile 'NODE\CHAIN\header.ckpt'))) 'checkpoint created with wrong anchor'
    $summary.steps += @{ wrong_anchor = 'PASS' }

    # 4. Correct external anchor creates trust=1.
    $n = Start-Node 'anchor' $profile $config 'checkpoint' '1' $anchor
    $text = Wait-Log $n 'event=header_checkpoint_write' 300
    Stop-Node $n
    Assert-True ($text -match 'trust=1') 'anchor did not produce trust=1'
    Assert-True (Test-Path (Join-Path $profile 'NODE\CHAIN\header.ckpt')) 'checkpoint missing after anchor'
    $summary.steps += @{ correct_anchor = 'PASS' }

    # 5. Fast restart accepts the checkpoint.
    $n = Start-Node 'fast' $profile $config 'fast' '0' ''
    $text = Wait-Log $n 'event=header_checkpoint_fast|header_checkpoint_full_fallback' 300
    Stop-Node $n
    Assert-True ($text -match 'event=header_checkpoint_fast') 'fast restart did not accept checkpoint'
    $summary.steps += @{ fast_restart = 'PASS' }
}
elseif ($Case -eq 'Lock') {
    $p1 = Start-Node 'lock-first' $profile $config 'fast' '0' ''
    $bind = Wait-Log $p1 'Starting node server' 300
    Assert-True ($bind -match 'Starting node server') 'first instance did not start'
    $lockConfig = Join-Path $WorkRoot 'config-lock.json'
    Write-Config $lockConfig $profile 29013 29014
    $p2 = Start-Node 'lock-second' $profile $lockConfig 'fast' '0' ''
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 15 -and -not $p2.Process.HasExited) {
        Start-Sleep -Milliseconds 250
        $p2.Process.Refresh()
    }
    $exited = $p2.Process.HasExited
    Stop-Node $p2
    Stop-Node $p1
    Assert-True $exited 'second instance did not fail fast (profile lock)'
    $summary.steps += @{ second_instance_fail_fast = 'PASS' }
}

$summary | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $WorkRoot 'summary.json') -Encoding UTF8
$summary | ConvertTo-Json -Depth 6
