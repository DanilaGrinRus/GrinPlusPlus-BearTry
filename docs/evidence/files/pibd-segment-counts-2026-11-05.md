# PIBD segment counters — primary log excerpts

Public excerpt of `sync-stages.log` files (original logs kept local).
Only the single `pibd_download_completed` event from each run is reproduced; no value changed.

## Candidate live run — GrinNode.exe `4039086C…`

Source: `work/merged-final-20261105/source/bin/Release/Release/logs/sync-stages.log` (line 17)
Profile: `work/merged-final-20261105/runs/live-merged`

```
[11/06/26 00:16:58.302+07:00] [info] event=pibd_download_completed received_segments=4999 required_segments=4999
```

PIBD started at height `3,321,360` (`17d0428f…`); activation pending removed at 00:18:12;
`Successfully validated TxHashSet` at 00:44:15; `PIBD state sync completed and validated` at 00:46:40.

## Earlier combined build — GrinNode.exe `16C24DB4…`

Source: `work/combined-kernel-auto-20261105/source/bin/Release/Release/logs/sync-stages.log` (line 13)
Profile: `work/combined-kernel-auto-20261105/runs/live-final-v8`

```
[11/05/26 16:52:03.723+07:00] [info] event=pibd_download_completed received_segments=4998 required_segments=4998
```

## Why the counts differ by one

The two runs started PIBD at different horizon heights (candidate `3,321,360`; combined
`3,320,640`). A one-segment difference is therefore expected and is not a defect. Each count
is **required = received** for its own run; neither value is transferred to the other build.
