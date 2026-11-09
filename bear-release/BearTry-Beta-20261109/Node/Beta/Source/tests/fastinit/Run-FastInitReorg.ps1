param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$BranchA,
    [Parameter(Mandatory=$true)][string]$BranchB,
    [Parameter(Mandatory=$true)][string]$WorkRoot
)

$ErrorActionPreference = 'Stop'
function Assert-True([bool]$c, [string]$m) { if (-not $c) { throw "ASSERT: $m" } }
function Write-Config([string]$path, [string]$data, [int]$rest, [int]$p2p, [int]$peerPort) {
    [ordered]@{
        DATA_PATH = $data
        LOGGER = @{ LOG_LEVEL = 'INFO' }
        P2P = @{ MAX_PEERS=1; MIN_PEERS=1; MIN_SYNC_PEERS=1; P2P_PORT=$p2p; P2P_PEER_PORT=$peerPort; PIBD_ENABLED=$false; PREFERRED_PEERS=@('127.0.0.1'); ALLOWED_PEERS=@('127.0.0.1') }
        SERVER = @{ REST_API_PORT = $rest }
    } | ConvertTo-Json -Depth 6 | Set-Content $path -Encoding UTF8
}
function Start-Node([string]$name, [string]$data, [string]$config, [string]$mode, [string]$bootstrap, [string]$anchor) {
    $dir = Join-Path $WorkRoot $name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $env:GRINPP_HEADER_CKPT=$mode; $env:GRINPP_HEADER_CKPT_BOOTSTRAP=$bootstrap
    $env:GRINPP_HEADER_CKPT_THREADS='1'; $env:GRINPP_HEADER_CKPT_SEGMENT='10000'
    $env:GRINPP_DISABLE_DNS_SEED='1'
    if ($anchor) { $env:GRINPP_HEADER_CKPT_TRUST_ANCHOR=$anchor } else { Remove-Item Env:GRINPP_HEADER_CKPT_TRUST_ANCHOR -ErrorAction SilentlyContinue }
    $env:TEMP=Join-Path $dir 'temp'; $env:TMP=$env:TEMP; New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null
    $log = Join-Path $data 'LOGS\Node.log'; Remove-Item $log -Force -ErrorAction SilentlyContinue
    $p=Start-Process -FilePath $Exe -ArgumentList @('--automated-testing','--no-wallet','--headless','--config-path',$config) -PassThru -RedirectStandardOutput (Join-Path $dir 'stdout.txt') -RedirectStandardError (Join-Path $dir 'stderr.txt') -WindowStyle Hidden
    return @{ Process=$p; Log=$log }
}
function Wait-Log($n,[string]$pat,[int]$sec=120) {
    $sw=[Diagnostics.Stopwatch]::StartNew()
    while($sw.Elapsed.TotalSeconds -lt $sec){
        Start-Sleep -Milliseconds 300
        if($n.Process.HasExited){break}
        if(Test-Path $n.Log){ $t=Get-Content $n.Log -Raw -EA SilentlyContinue; if($t -match $pat){return $t} }
    }
    if(Test-Path $n.Log){ return (Get-Content $n.Log -Raw -EA SilentlyContinue) } else { return '' }
}
function Stop-Node($n){ if($n -and $n.Process -and -not $n.Process.HasExited){Stop-Process -Id $n.Process.Id -Force -EA SilentlyContinue}; Start-Sleep -Milliseconds 500 }

if ($BranchA -match 'live-merged' -or $BranchB -match 'live-merged' -or $BranchA -match 'RC[12]' -or $BranchB -match 'RC[12]') { throw 'Refusing live/RC profiles' }
if (Test-Path $WorkRoot) { Remove-Item $WorkRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$a=Join-Path $WorkRoot 'A'; $b=Join-Path $WorkRoot 'B'; Copy-Item $BranchA $a -Recurse -Force; Copy-Item $BranchB $b -Recurse -Force
$ac=Join-Path $WorkRoot 'a.json'; $bc=Join-Path $WorkRoot 'b.json'; Write-Config $ac $a 28613 28614 28714; Write-Config $bc $b 28713 28714 28614

# B: obtain lab aggregate, then create trust=1 checkpoint for the exact B state.
$n=Start-Node 'hashB' $b $bc 'hash' '0' ''; $t=Wait-Log $n 'event=header_checkpoint_hash_only' 120; Stop-Node $n
Assert-True ($t -match 'aggregate=([0-9a-fA-F]{64})') 'B aggregate missing'; $anchor=$Matches[1]
Remove-Item (Join-Path $b 'NODE\CHAIN\header.ckpt') -Force -EA SilentlyContinue
$n=Start-Node 'ckptB' $b $bc 'checkpoint' '1' $anchor; $t=Wait-Log $n 'header_checkpoint_write|trust_anchor' 120; Stop-Node $n
Assert-True ($t -match 'trust=1') 'B checkpoint not trusted'

# A source and B target, ordinary P2P loopback.
$aNode=Start-Node 'A' $a $ac 'off' '0' ''; $t=Wait-Log $aNode 'Starting node server' 120; Assert-True ($t -match 'Starting node server') 'A did not start'
$bNode=Start-Node 'B' $b $bc 'fast' '0' ''; $t=Wait-Log $bNode 'Fork detected|header_checkpoint_fast' 120
# Wait until B's candidate file reaches A's branch tip (real branch switch).
$deadline=[DateTime]::UtcNow.AddSeconds(60)
while([DateTime]::UtcNow -lt $deadline){
    Start-Sleep -Milliseconds 500
    if($bNode.Process.HasExited){break}
    $fa=Get-Item (Join-Path $a 'NODE\CHAIN\candidate.chain') -EA SilentlyContinue
    $fb=Get-Item (Join-Path $b 'NODE\CHAIN\candidate.chain') -EA SilentlyContinue
    if($fa -and $fb -and $fa.Length -eq $fb.Length){break}
}
Start-Sleep -Seconds 1
Stop-Node $bNode; Stop-Node $aNode

function Tip($path){ $fs=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite); try{$fs.Position=$fs.Length-32; $buf=New-Object byte[] 32; [void]$fs.Read($buf,0,32); return ([BitConverter]::ToString($buf)).Replace('-','')} finally{$fs.Close()} }
$tipA=Tip (Join-Path $a 'NODE\CHAIN\candidate.chain'); $tipB=Tip (Join-Path $b 'NODE\CHAIN\candidate.chain')
Assert-True ($tipA -eq $tipB) "reorg did not reach A tip A=$tipA B=$tipB"
Write-Output "reorg PASS A=$tipA B=$tipB"
