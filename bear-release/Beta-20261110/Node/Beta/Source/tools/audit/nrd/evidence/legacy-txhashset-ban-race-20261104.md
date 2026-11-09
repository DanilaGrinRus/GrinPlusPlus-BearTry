# Legacy TxHashSet: first-archive evidence and late-response ban race

Date: 2026-11-04 (Asia/Bangkok)
Scope: read-only live evidence for PID 3680 + targeted source diagnostics/no-ban fix.
No build, no commit, no interruption of the live process or line1 profile.

## Live run

- PID 3680, `<workspace>\.tmp\build-33\GrinNode.exe`, started 01:05:14.
- `--floonet --no-wallet --headless --config-path <workspace>\work\runs\task2-legacy-public\servers.json`
- PIBD disabled; legacy txhashset path; REST loopback `127.0.0.1:25650`.
- At 03:53: `txhashset_download`, tip height 0, 2 healthy peers (66.154.127.84 at
  3,321,739 caps 223; 195.201.2.51 at 3,321,814 caps 95).
- Second partial archive: `tmp\txhashset_451e47029b0b.zip`, 2,633,498,624 B at
  04:01:54 and still growing (2,678,194,176 B at 04:03:25), i.e. second transfer
  is active; earlier 1.025 GB sample was not the end state.

## First archive (peer 80.228.193.72)

Timeline from `cpp-profile\LOGS\Node.log`:
- 01:07:10 start `Downloading TxHashSet from Connection{80.228.193.72}`
- 02:32:12 `Downloading successful`
- 02:32:17 `Extract(37) - Successfully extracted zip file`
- 02:32:17 `LoadFromZip(92) - Failed to load: Rewind: Failed to rewind to size 3354438`
- 02:32:17 `Invalid TxHashSet received.`
- 02:32:17 `Banning peer 80.228.193.72 for 'BadTxHashSet'`

The archive zip was removed after extraction and the staging directory was
removed by the processor on failure; only the genesis TXHASHSET tree remains
(mtime 23:45). The first archive cannot be re-tested from this run.

**Classification:** archive-vs-local cause is NOT VERIFIED. `LoadFromZip`
swallows the rewind exception and `ProcessTransactionHashSet` maps it to INVALID,
which the old pipe treats as peer evidence.

## Late-response race (three Abusive bans)

Log:
- 01:07:10 archive download starts from 80.228.193.72.
- 01:07:58 `Rotating txhashset sync peer: 66.154.127.84` while 80 transfer is active.
- 01:08:14 `Received TxHashSet from Connection{103.7.54.154} when already processing another.`
  followed by `Banning peer 103.7.54.154 for 'Abusive'`.
- 01:08:54 same for 96.44.159.174.
- 01:09:04 same for 185.177.219.185.

Code before fix:
- `TxHashSetPipe::ReceiveTxHashSet`: processing=true -> `BanPeer(Abusive)`.
- old `StateSyncer::IsStateSyncDue`: 10 s with downloaded==0 -> rotate;
  `RequestState` had a 30 s guard, so overlapping requests were still possible.

## Source changes (uncommitted, no build)

- `src/P2P/Pipeline/TxHashSetPipe.cpp`:
  - late/unrequested or already-processing archive is now ignored, receives
    disabled, **no ban**;
  - transmission end, INVALID result and exceptions no longer ban; log states
    peer validity is unproven/classification required;
  - implausible size still rejected/banned (reliable protocol violation).
- `src/P2P/Sync/StateSyncer.cpp`: initial no-progress rotation window 10 s -> 120 s
  to reduce overlapping requests.
- `src/PMMR/TxHashSetManager.cpp`: rewind diagnostics now log header hash/height,
  target kernel/output counts, actual MMR sizes and which MMR failed.
  Failed archive/staging is still not preserved (remaining gap).

Hashes:
- TxHashSetPipe.cpp `8285D05313091B1FC7388083BB2289773A69FEA376C4F0EBBE15019F357F9B29`
- StateSyncer.cpp `3CA320D6339E7C9E3679EF9A5908030DE307B633C106168BE335F073F39E7B2D`
- TxHashSetManager.cpp `0029777BD7CB80E3C289AC031879CEAC990F00770CDB240656D011C7CFF467D1`

## Remaining work

1. Exact request/response identity tracking (peer+archive hash+height), not just
   “processing” state; ignore stale answers and preserve anti-push protection.
2. Preserve the failed zip/staging or a bounded digest/manifest for diagnosis.
3. Partial-download no-progress watchdog (network timeout alone is insufficient;
   old check only special-cased downloaded==0).
4. Regression test for late archive/no-ban; needs Connection/Pipeline fixture.
5. Re-run legacy public fallback on a new build (not this live PID) before
   claiming first-ban or race behavior PASS.
