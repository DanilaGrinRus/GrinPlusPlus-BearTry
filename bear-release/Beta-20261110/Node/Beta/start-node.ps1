param(
  [Parameter(Mandatory=$true)][string]$ConfigPath
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$node = Join-Path $root 'GrinNode.exe'
if (-not (Test-Path $node)) { throw "GrinNode.exe not found next to launcher" }
if (-not (Test-Path $ConfigPath)) { throw "config not found: $ConfigPath" }

# Beta acceptance was performed with --no-wallet. Do not expose the
# unaccepted wallet+node mode through the Beta launcher.
Write-Host "Starting Beta node-only (wallet disabled): $node"
& $node --mainnet --headless --no-wallet --config-path $ConfigPath
exit $LASTEXITCODE
