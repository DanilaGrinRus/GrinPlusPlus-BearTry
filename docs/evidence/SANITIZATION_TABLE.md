# Sanitization table

| public file | original SHA-256 | public SHA-256 | redactions |
|---|---|---|---|
| tests-full-final.log | `efdd1b9ea8aa6c5c1a5fffd48b6d06c80a973ade3b5aa86671028a3e7bf02212` | `df27567f88dc622147062fcd34486990682ec4e633bc8e277534014f3e990fda` | {'<candidate-source>': 4, '<ip>': 5} |
| target-final.json | `c261c79121240951e0c4c29e40b96ba43e7040db3fbafb1e110e3f4f5a455c2d` | `97150bee835e4cf2c0c0021fcf380aa61c7184f82bb85b86638986a45c949e19` | {} |
| tests-fullsweep.log | `212f3204c8e6a34001884886abcac59eee737d0d8be4f1b4d29883eb1e8f04bb` | `ee909fa5af35c0ced047c2bb2a865aa97435a8a330b41489c7bab9cf981145c8` | {'<candidate-source>': 2} |
| build-final.log | `64644cdd401dabeb3c39ef986684a5dab321c98e83719d027b473a16d8320178` | `4e9355b81672a41e7ac1d42f6a7e4d0b18edbc14cb2bc3737876e5d2feaff0dc` | {'<candidate-source>': 48, '<work-root>': 4} |
| configure.log | `243cab979af339309260c5ba599b761bee28dc1d1151fbf96a8476d81d7a3038` | `36a6946e7e6f210f19ff8aac681f880f49f3d223ddfb5ed6f2a2e4160257d9df` | {'<host>': 4, '<VS2022-BuildTools>': 3} |
| nrd-kill-manifest.json | `6e3f5feac79daf78d627a79c222d11acb0469d8d19d2a5ad97b508be1b5ad6cf` | `ee9d53183172df921af6538bf5367af3354e073cd048ce3da906133cd343d636` | {'<candidate-source>': 1, '<work-root>': 2} |
| nrd-kill-child.stdout.log | `fd06037b327cf50c6383bfd837a919f6430c691a660f9233126cd09a62e23725` | `ffc3acecff1f172778ae1b213c22848b40c10e187e105f208b4bd5b8970b145b` | {} |
| nrd-kill-recovery.stdout.log | `eaf152d52cfd61a90198f7eaf35abbcf6aec53cf49537a8fc65a3713a21c1fc0` | `c22f1ccea0c926af1ec2556161c6d7a549fb7f16ab95c6d01cc0cbc4bae2849f` | {} |
| candidate-live-floonet-2026-11-05.md | curated from Node.log + run metadata (see ORIGINALS.md) | `8775388afe336f54758f034cd195274f34bab19d04600c56ee13302477c20607` | absolute paths and peer IPv4s removed; no values changed; updated for 4999/4999 |
| candidate-live-node-log-excerpt-2026-11-05.log | `a1145d0ff79ace243e49b423d1983bf66fd75dab59a7ebc6f8c261bfe0fb5f99` (original Node.log, 85,813 B) | `84dfea615ff3aeba2b90ef6456f98b62fe9b15a45fc1eff6b3109d954beb6826` | absolute workspace paths and peer IPv4s replaced; 36 selected log lines |
| pibd-segment-counts-2026-11-05.md | curated excerpts of two local `sync-stages.log` files (see ORIGINALS.md) | `8be15cc4300b729e0e2c11126a543fa0b51df20ecd5d0ef8d022c55ec729edf0` | absolute paths removed; one `pibd_download_completed` line per run; no values changed |
