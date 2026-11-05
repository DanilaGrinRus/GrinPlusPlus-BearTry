# Evidence originals and redaction record

The public files in [`files/`](files/) are sanitized copies. This page records
where each original came from, its size and SHA-256, and what was redacted.
Paths are relative to the preparation workspace (not absolute developer paths).
Maintainers can re-check the hashes without publishing machine-specific data.

## Redaction policy

Redacted before publication:

- absolute local paths in logs and manifests;
- peer IP addresses and host names;
- local user names, process IDs, ports that identify a private test machine;
- profile paths and wallet-specific data;
- any API secret or seed material (none is known to be present; files were
  scanned before publication).

Kept unchanged: timestamps, heights, hashes, assertion counts, exit codes,
binary SHA-256 values, dependency revisions and test names. Redaction never
changes a result value.

## Originals

| Public file | Original artifact | Size | SHA-256 of original |
|---|---|---|---|
| `files/tests-full-final.log` | `work/merged-final-20261105/logs/tests-full-final.log` | 26 980 B | `EFDD1B9EA8AA6C5C1A5FFFD48B6D06C80A973ADE3B5AA86671028A3E7BF02212` |
| `files/target-final.json` | `work/merged-final-20261105/logs/target-final.json` | 731 B | `C261C79121240951E0C4C29E40B96BA43E7040DB3FBAFB1E110E3F4F5A455C2D` |
| `files/tests-fullsweep.log` | `work/merged-final-20261105/logs/tests-fullsweep.log` | 925 B | `212F3204C8E6A34001884886ABCAC59EEE737D0D8BE4F1B4D29883EB1E8F04BB` |
| `files/nrd-kill-manifest.json` | `work/merged-final-20261105/logs/nrd-kill-evidence/kill-20261105-235125/manifest.json` | 905 B | `6E3F5FEAC79DAF78D627A79C222D11ACB0469D8D19D2A5AD97B508BE1B5AD6CF` |
| `files/nrd-kill-child.stdout.log` | `…/kill-20261105-235125/child.stdout.log` | 5 019 B | `FD06037B327CF50C6383BFD837A919F6430C691A660F9233126CD09A62E23725` |
| `files/nrd-kill-recovery.stdout.log` | `…/kill-20261105-235125/recovery.stdout.log` | 208 B | `EAF152D52CFD61A90198F7EAF35ABBCF6AEC53CF49537A8FC65A3713A21C1FC0` |
| `files/build-final.log` | `work/merged-final-20261105/build-final.log` | 5 407 B | `64644CDD401DABEB3C39EF986684A5DAB321C98E83719D027B473A16D8320178` |
| `files/configure.log` | `work/merged-final-20261105/configure.log` | 4 737 B | `243CAB979AF339309260C5BA599B761BEE28DC1D1151FBF96A8476D81D7A3038` |
| `files/combined-live-floonet-2026-11-05.md` | `work/combined-kernel-auto-20261105/FINAL_REPORT.md` | 6 391 B | `3734E46E73E204F96996FBAA456DDA4D9FB69BE291D056C62CDCE3EC494AE08B` |
| `files/candidate-source-manifest-final.sha256` | `evidence-index/candidate-source-manifest-final.sha256` | 165 660 B | `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627` |

## Local-origin evidence (not published)

| Artifact | Size | SHA-256 |
|---|---|---|
| `work/kernel-index-fast-20261105/REPORT.md` | 14 004 B | `250EB1EFB560C69FC22F652482C84D3876523D55E4A45D6A44EEB46E58EB9C70` |
| `work/auto-workers-pibd-20261105/REPORT.md` | 6 894 B | `E3D0A7A1F10FD583B159BB3852C5B6CBC07E4D63536A8A29808C84E2E181390D` |
| `work/combined-kernel-auto-20261105/FINAL_REPORT.md` | 6 391 B | `3734E46E73E204F96996FBAA456DDA4D9FB69BE291D056C62CDCE3EC494AE08B` |
| `work/header-checkpoint-20261105/FINAL_STATUS_FASTINIT.md` | 8 895 B | `B251B33CCD1A689E80723307C8FFABEDFECCE8745EA01FCE95C7EC0AEF8A29B8` |
| `work/merged-final-20261105/merged-fastinit-current.patch` | 714 136 B | `7771DD7A6C3A256747D9626B7A52BF859A04E5CB576DD0A3FD63758CD8DB65BA` |

Additional local-only evidence (profiles, run logs, API check artifacts,
build logs) is listed in [`INDEX.md`](INDEX.md) and remains with the maintainers.

## Stale manifest

`work/merged-final-20261105/source-manifest.sha256` was generated before five
files were edited and the binaries rebuilt, so it does not describe the final
candidate. It is kept only as provenance history. The authoritative source
identity is `files/candidate-source-manifest-final.sha256`
(`964B2DD8…`, 1528 files).
