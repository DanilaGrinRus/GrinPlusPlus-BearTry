# Public evidence — live Floonet run on the accepted candidate

Source: local `work/merged-final-20261105/runs/live-merged/` (original logs kept local).
Public excerpt: `docs/evidence/files/candidate-live-node-log-excerpt-2026-11-05.log` (paths and peer IPs redacted).
Date of run: 2026-11-05 23:33 – 2026-11-06 00:55 (Asia/Bangkok).
Executable: `GrinNode.exe` SHA-256 `4039086CE339106DE37D7C6A98D54873171458699507DB18E938AFD938C80EFC` (candidate build, 2026-11-05 23:48:46).
Test binary: `Tests.exe` SHA-256 `A9594508FBDF5B48AA123A862CBB68B20882AD98BB8AA437A9E9A9C60D4FAC8C`.

Profile: fresh isolated Floonet profile inside the work tree; `--floonet --headless --no-wallet`; REST 15113, P2P 15114, peer port 13414; 10 outbound peers. No fixed peer list.

## Timeline (from the sanitized log excerpt)

| Time (+07) | Event |
|---|---|
| 23:33:09 | first start on the fresh profile; index generation 1 (height 0) |
| 23:48:54 | restart with the final candidate binary |
| 23:52:32 | restart; `kernel_index_check result=current`, generation 1 (height 0) |
| 00:14:17 | PIBD state sync starts at height **3,321,360**, hash `17d0428f…`, **10 capable peers** |
| 00:16:58 | `pibd_download_completed received_segments=4999 required_segments=4999` (primary `sync-stages.log`) |
| 00:18:12 | PIBD staging complete; activation file removed |
| 00:18:21 | `ValidateTxHashSet` for block `17d0428f…` begins |
| 00:41:33 | range-proof phase ends: **2,948,812 / 2,948,812 proofs verified**, 1 worker, 1,215,538 ms, 2,425.9 proofs/s |
| 00:44:15 | `Successfully validated TxHashSet` |
| 00:45:10 | `kernel_index_check` at the PIBD state: chain_height 3,321,360, kernel/txhashset 3,357,330 |
| 00:46:20 | KernelIndex generation 2 rebuild entries: 3,357,330 kernels in 70,836 ms |
| 00:46:30–33 | rebuild commit 9,902 ms, verify 2,309 ms, publish OK |
| 00:46:40 | `PIBD state sync completed and validated` |
| 00:47:09 | TxHashSet archived at height 3,321,360 for serving (serving itself disabled by default) |
| 00:53:13 | graceful shutdown (`Closing Grin++ v1.2.9`) |
| 00:53:21 | graceful restart |
| 00:54:30 | startup `kernel_index_check state=present generation=2 indexed_height=3,324,510 kernels=3,360,496` |
| 00:54:34 | `kernel_index_check result=current … tip=289d9465…` — **no rebuild after restart** |
| 00:55 | status API: `sync_status=FULLY_SYNCED`, chain height **3,324,513**, network height 3,324,513 |
| 00:55:53 | graceful shutdown; node context deleted |

`bans_fraud_height=0`, `bans_invalid_data=0` at the final status check.

## Findings / limitations

- During block catch-up, transient `GetOutputType: Output Commitment … not found` exceptions and block-request deadline warnings were logged. They were retried; no peer was banned; the node still reached `FULLY_SYNCED`. The exact cause of those transient rejections on this peer/tip was **not established**; they are not claimed as fixed, only as non-fatal in this run.
- The run used the default single range-proof worker (worker selection log: `workers=1 … default one worker`). Auto-worker mode was not exercised.
- Mainnet was not run. Adversarial live peers, power loss, reorg and multi-day stability were not tested.
- The run was stopped gracefully by the surrounding test harness; the last status snapshot is `FULLY_SYNCED` at height 3,324,513 and the chain checkpoint file (00:54:58) records height 3,324,514.
- The environment had `GRINPP_HEADER_CKPT=write` / bootstrap intent recorded earlier in the run metadata, but the log shows no `header_checkpoint_*` events during this run; FastInit is therefore not claimed as exercised.

## Verdict

**PASS for the documented Floonet scenario** (fresh profile → PIBD → validation → index → catch-up → graceful restart with current index → `FULLY_SYNCED` → graceful stop) on the exact candidate binary. **Mainnet: NOT TESTED.**
