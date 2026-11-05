# Public evidence excerpt — combined KernelIndex + multi-peer PIBD live Floonet run

Source: local `work/combined-kernel-auto-20261105/FINAL_REPORT.md` (original kept local).
Date of run: 2026-11-05 (Asia/Bangkok).
This is a public, redacted excerpt. Peer IP addresses and local filesystem paths were removed; no result was changed.

## Build identity

| Artifact | SHA-256 |
|---|---|
| Combined `GrinNode.exe` | `16C24DB404702595EDB4CEDA7A057D2C187339FB92664473D211DFDB5F3FF3A2` |
| Combined `Tests.exe` | `790A5F34F2CC4978EAF2143D04BA414AF96FE9FE83D60AA036197FD83AD0391B` |
| Auto-workers patch `auto-workers-v8.patch` | `736C0A87623C15EECB5005C98900603C55469C303A87F3A4D67DC6284FAD8A4F` |

## Deterministic tests

- Full suite on `790A5F34…`: **All tests passed — 4982 assertions / 373 test cases, exit 0**, 71.6 s.
- KernelIndex orphan: PASS — 17 assertions.
- KernelIndex write faults: PASS — 81 assertions.
- KernelIndex after TxHashSet install (`[pibd]`): PASS — 25 assertions.
- Kill harness entry/state: PASS — 219 assertions each, recovery exit 0.
- PibdScheduler multi-peer: PASS — 68 assertions / 9 cases.
- PibdSync bad segment: PASS — 9 assertions.
- BlockPipe no-false-ban: PASS — 41 assertions / 6 cases.
- `PibdSegmentService::FullSweepPpCkpt`: PASS 6 assertions but fixture-limited (bitmap 0/7, output 0/1678, rangeproof 0/1678, kernel 0/1641) — not a full-state sweep.
- NRD `RebuildKillChild/Recovery` external harness: omitted in that run.

## Isolated live Floonet

Profile: fresh, isolated inside the work tree. EXE: `16C24DB4…`. Ports: REST 15013, P2P 15014. Command: `GrinNode.exe --floonet --headless --no-wallet --config-path ...`.

| Stage | Result |
|---|---|
| Header sync | 0 → 3,324,027; 10 connections |
| PIBD start | height 3,320,640, **9 peers** |
| PIBD download | **4,998 / 4,998 segments** |
| PIBD activation write | 95.2 s, success |
| State validation | kernel history 64 s, kernel sums 146 s, range proofs 1,379,152 ms (1 worker), kernel signatures 197 s; total 1,987,361 ms |
| `ProcessExistingTxHashSet` | state activation for block `2ca6c3ff…`; `BlockChain::ProcessExistingTxHashSet` called at horizon |
| Initial KernelIndex | generation 2, **3,356,610 kernels**, 101 s, published tip `2ca6c3ff…` |
| Catch-up | `fully_synced` at 17:30:33, height 3,324,064 |
| Restart | multiple graceful restarts; each startup `kernel_index_check result=current`, generation 2; no rebuild after initial build |
| Final index/tip | height 3,324,082, index current, kernels 3,360,066, chain tip `6cf70a50…`, no full rebuild |

Final no-network index check (REST 15015):
`kernel_index_check result=current generation=2 indexed_height=3324082 kernels=3360066 chain_height=3324082 chain_kernels=3360066 txhashset_kernels=3360066`.

## Peer recovery / no false ban

- 10 connections, `pibd_started peers=9`; no header-timeout event in the successful run.
- An earlier diagnostic on the same candidate recorded header-sync timeouts and peer switching without a ban.
- Final counters: `bans_fraud_height=0`, `bans_invalid_data=0`.
- Deterministic scheduler tests cover wrong-peer rejection without segment loss, late/duplicate responses, send failure and disconnect reassignment.

## Limitations (verbatim from the original report)

- `PibdSegmentService::FullSweepPpCkpt` hidden fixture absent.
- NRD rebuild kill harness omitted in that run.
- Real power loss (kill harness ≠ power loss), real RocksDB failure.
- Live multi-peer wrong-segment/ban behaviour under adversarial peers was not tested; only a normal 9-peer download and the deterministic scheduler matrix.
- Full-state 1/2/3/4/auto worker benchmark remains open; default stays 1.
- No Mainnet run. No RC2/RC1 integration. PIHD excluded.
