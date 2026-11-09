# Beta delivery — node-only boundary

The Beta acceptance was performed with `--no-wallet`. The default `GrinNode.exe`
startup enables the wallet (`src/Server/opt.h`), which is not covered by the Beta
node acceptance. Therefore:

- use `start-node.ps1 -ConfigPath <config.json>` for the accepted Beta mode;
- the launcher always passes `--mainnet --headless --no-wallet`;
- do not use a plain `GrinNode.exe` launch without `--no-wallet` for Beta evidence;
- wallet+node remains a separate testnet acceptance track (W1), not part of this Beta.

Exact pair hashes must be recorded before distribution.
