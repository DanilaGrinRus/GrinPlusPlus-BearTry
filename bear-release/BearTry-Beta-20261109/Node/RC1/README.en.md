# Node Beta1 — preliminary RC1

**Status:** a candidate for further testing, not an accepted Beta1 or a stable release. Full PIBD from scratch, Mainnet and restarts at sync stages are not verified on this EXE.

`GrinNode.exe` — Windows x64, SHA-256 `CD941F325B6E7595E9A777DDA90B486BCAFB0473322CBD95E3FCA5133344FD48`.

To start the **Floonet** node without a wallet, open `START_FLOONET_NODE.cmd`. It creates its profile in `node-data` next to the launcher; the folder must be writable. Logs go to `logs`. Running the EXE without arguments defaults to Mainnet and enables the wallet.

- `source/`: node, API and test sources needed for a self-build. Some old auxiliary tools tied to the previous machine are omitted; see `source/OMITTED_LOCAL_TOOLS.md`.
- `dependencies/sysroot/`: prebuilt Windows x64 libraries.
- `BUILD.ru.md`: build and test instructions in Russian.
- `evidence/`: test evidence; older runs and their paths are historical, not runtime configuration.
- `docs/`: audit and API-matrix snapshots; relative links in historical snapshots may not work after relocation.
- `SHA256SUMS.txt`: package file hashes; the user-created archive and runtime data are not included.

MSVC Release /W4 /WX: **362 test cases / 4,886 assertions PASS** on Tests SHA `29F068B92DD575139AED5B2CDC0DC38A274C35B6EAA037A9AE9A4744A65E36E5`. Hidden harness scenarios and the live fixture were excluded; two external fixtures were absent and their checks skipped. PIBD segment display was tested synthetically; a live download remains unverified. The launcher, HTTP health and graceful shutdown passed in a relocated folder with spaces and no external peers.

Owner RPCs `compact_chain`, `validate_chain`, `reset_chain_head` and `invalidate_header` are not implemented in this candidate. Interruption of compaction and restart after actual deletion remain unverified. API and logging at all sync stages still require live checks.

The package requires compatible Windows x64 and a writable directory; self-building also requires CMake and MSVC. Two starts and clean shutdowns were tested on the original computer after relocation to a path with spaces and Cyrillic characters, a separate environment was reported by the user to start and sync headers; that copy's SHA was not measured and network shares were not tested. The other supplied third-party binaries have not been independently rebuilt from source.

The static RocksDB used by this build was rebuilt with Windows UTF-8 paths and portable settings; the included shared variant is not used by the node and was not separately tested.

On this SHA, a clean Floonet node without a fixed peer list reached 5 connections and began header sync (512 of 3323361); complete PIBD and catch-up remain unverified. DNS seeds follow Grin 5.5.2; outbound TCP distinguishes a five-second timeout from OS error codes. Invalid magic bytes remain rejected and diagnostics include received and expected bytes. Individual peer errors remain possible. The NRD differential ran without the external Rust oracle; this is not proof of Rust parity.

Running RC1 snapshot, 2026-11-05 06:02 UTC+07: 8 peers, headers 45568 → 49664 in 5 seconds; chain height 0, header sync in progress. This observation is not a completed sync. See evidence/peer-repair/rc1-live-observation.json.
