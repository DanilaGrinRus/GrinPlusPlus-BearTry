# Node HTTP and JSON-RPC API

This document describes the node API of the `node-beta.1-candidate` tree. The
source of truth is the code:

- REST `/v1` routes: `src/Server/Node/NodeRestServer.cpp` and
  `src/Server/Node/API/*.cpp`.
- JSON-RPC foreign/owner registration: `src/API/Node/NodeServer.cpp` and
  `src/API/Node/Handlers/*.h`.
- Access control: `include/Net/Servers/RPC/ApiAccess.h`,
  `src/Net/Servers/RPC/ApiAccess.cpp`, `include/Net/Servers/RPC/RPCServer.h`.

Status labels used here:

- **IMPLEMENTED** — the handler exists and is exercised by automated tests
  and/or local HTTP checks.
- **PARTIAL** — implemented, but the schema, error matrix, permissions or live
  phases were not fully verified against the reference.
- **NOT IMPLEMENTED** — no handler is registered; do not expect a fake success.

The reference for parity comparisons is **Rust Grin 5.5.2**
(`reference/grin-5.5.2`, `api/src/foreign_rpc.rs`, `api/src/owner_rpc.rs`). This
fork intentionally adds C++ extensions; a difference is not automatically a bug,
but it must be documented for consumers.

## Server layout and authentication

Both HTTP REST and JSON-RPC are served by the same local listener on
`SERVER.REST_API_PORT` (default `3413` Mainnet, `13413` Floonet). JSON-RPC
endpoints are `POST /v2/foreign` and `POST /v2/owner`; bodies are JSON-RPC 2.0
(`{"jsonrpc":"2.0","method":"…","params":[…],"id":1}`). REST responses are plain
JSON (not JSON-RPC).

On first start the node creates a 64-hex-character secret at
`<DATA_PATH>/.api_secret`. It is never logged and never placed in URLs.
Windows: the file must have a protected owner-only DACL (owner + SYSTEM);
POSIX: mode `0600`. A missing, malformed or insecure secret fails closed for
PRIVATE/ADMIN methods; the node does not silently replace it.

Authentication headers (either is accepted):

```
Authorization: Bearer <secret>
X-API-Secret: <secret>
```

The request `Host` must be loopback (`127.0.0.1`, `localhost`, `::1`); an
`Origin` header, when present, must be a loopback origin or `null`. Binding the
API to `EServerType::LOCAL` is not authentication by itself.

| Level | Used for | Failure |
|---|---|---|
| PUBLIC | read-only chain/peer data, health | no secret needed |
| PRIVATE | node status/peers/config reads | JSON-RPC `-32001`, REST `401` |
| ADMIN | config writes, peer ban/unban, resync, shutdown | JSON-RPC `-32001`, REST `401` |

`ban_peer`/`unban_peer`, `update_config`, `/v1/resync`, peer ban/unban REST
calls and `/v1/shutdown` are ADMIN. `get_status`, `get_peers`,
`get_connected_peers` and `get_config` are PRIVATE. Foreign read methods are
PUBLIC.

## REST `/v1`

`GET /v1/` returns a plain JSON list of the routes below. Several read handlers
do not enforce the HTTP method themselves; use the documented method anyway and
do not build clients that rely on unspecified methods.

| Method and path | Purpose | Status | Notes / errors |
|---|---|---|---|
| `GET /v1/health` | API/process readiness | **IMPLEMENTED** | `{"status":"starting"\|"ready"}`. Available before the chain is open, typically first 200 at `0.56–0.62 s`. `ready` means handlers are registered, not that the node is synced. Non-GET → 404. |
| `GET /v1/status` | Node, chain and sync state | **PARTIAL** | Legacy fields plus a Rust-compatible `rust` object; 500 if no tip. Not guaranteed during the first milliseconds (404 until registration) and observed to exceed 5 s once after state validation; cause not established. |
| `GET\|POST /v1/resync` | Resync chain and unban all peers | **IMPLEMENTED** | ADMIN. Side effects: `ResyncChain` + `UnbanAllPeers`; 401 without secret. |
| `GET /v1/headers/<hash\|height\|commit>` | One header | **IMPLEMENTED** | 64-hex = hash, 66-hex = commitment, otherwise decimal height. 400 `HEADER NOT FOUND`. |
| `GET /v1/blocks/<hash\|height\|commit>[?compact]` | One full or compact block | **IMPLEMENTED** | 400 `BLOCK NOT FOUND`; `?compact` returns a compact block. |
| `GET /v1/chain` | Confirmed tip | **IMPLEMENTED** | `{height,last_block_pushed,prev_block_to_last,total_difficulty}`; 500 if no tip. |
| `GET /v1/chain/outputs/byids?id=<commit>[,<commit>]…` | Output locations by commitment | **IMPLEMENTED** | Array of `{commit,height,mmr_index}`; unknown ids are omitted. 400 on malformed input. |
| `GET /v1/chain/outputs/byheight?start_height=N&end_height=M` | Blocks with outputs in a height range | **IMPLEMENTED** | `end_height` defaults to `start_height` when absent or before start. 400 on invalid numbers. |
| `GET /v1/peers/all` | Known peers | **IMPLEMENTED** | Array of peer objects. |
| `GET /v1/peers/connected` | Connected peers | **IMPLEMENTED** | Array of connected-peer objects. |
| `GET /v1/peers/diagnostics` | Aggregate peer/download diagnostics | **IMPLEMENTED** | `totals`, `banned`, `connected`, `download`, `versions`. Addresses are operational data; treat them as sensitive when publishing. |
| `GET /v1/peers/<ip>` | One known peer | **IMPLEMENTED** | Returns an **object** on the newer candidates (the older contract returned a single-element array). 404 `Peer not found.` |
| `POST /v1/peers/<ip>/ban` | Manual ban | **IMPLEMENTED** | ADMIN; empty success body. Ports are parsed but peer identity is IP-only. |
| `POST /v1/peers/<ip>/unban` | Manual unban | **IMPLEMENTED** | ADMIN; includes peers that exist only in the DB. |
| `GET /v1/txhashset/roots` | Output/range-proof/kernel roots of the tip | **IMPLEMENTED** | 404 if no tip. |
| `GET /v1/txhashset/lastkernels?n=N` | Last N kernel hashes | **IMPLEMENTED** | Array of `{"hash":"…"}`; default 10, capped at 1000. Older builds returned strings. 400 unless the query starts with `n=`; 500 if TxHashSet is absent. |
| `GET /v1/txhashset/lastoutputs?n=N` | Last N output hashes | **IMPLEMENTED** | Array of `{hash}`; same limits. |
| `GET /v1/txhashset/lastrangeproofs?n=N` | Last N range-proof hashes | **IMPLEMENTED** | Array of `{hash}`; same limits. |
| `GET /v1/txhashset/outputs?start_index=N&max=M` | Unspent output page | **IMPLEMENTED** | `{highest_index,last_retrieved_index,outputs[]}`; `max` capped at 1000. |
| `POST /v1/shutdown` | Graceful shutdown | **IMPLEMENTED** | ADMIN; non-POST → 404. Empty body on success. |

### Example: `/v1/status` (abridged)

```json
{
  "protocol_version": 1000,
  "user_agent": "Grin++ 1.2.9",
  "sync_status": "FULLY_SYNCED",
  "waiting_reason": "",
  "db_repaired": 0,
  "db_last_repair": 0,
  "block_sync": {
    "requests": 0, "timeouts": 0, "reassignments": 0,
    "cooldown_reassignments": 0, "retries": 0,
    "bans_fraud_height": 0, "bans_invalid_data": 0
  },
  "state": {"downloaded": 0, "download_size": 0, "processing_status": 100, "sync_failed": false},
  "network": {"height": 3324082, "total_difficulty": 0, "num_inbound": 0, "num_outbound": 2},
  "chain": {"height": 3324082, "hash": "…", "previous_hash": "…", "total_difficulty": 0},
  "header_height": 3324082,
  "rust": {
    "chain": "floonet",
    "protocol_version": 1000,
    "user_agent": "Grin++ 1.2.9",
    "connections": 2,
    "tip": {"height": 3324082, "last_block_pushed": "…", "prev_block_to_last": "…", "total_difficulty": 0},
    "sync_status": "no_sync"
  },
  "tip": {"height": 3324082, "last_block_pushed": "…", "prev_block_to_last": "…", "total_difficulty": 0},
  "connections": 2,
  "chain_type": "floonet",
  "sync_status_rust": "no_sync"
}
```

`pibd` (`received_segments`, `required_segments`) appears only while PIBD has
started. `sync_info` appears only when the current phase has real data.

## Foreign JSON-RPC (`POST /v2/foreign`)

All 13 method names from Rust Grin 5.5.2 are registered. Registration is not
schema parity; per-method parameter, output and error verification against the
reference is **PARTIAL** unless stated otherwise.

| Method | Params | Result | Status |
|---|---|---|---|
| `get_header` | `[height, hash, commit]` — at least 3, nullable (extra ignored) | `{"Ok": <header>}` | **IMPLEMENTED / PARTIAL** (schema not line-by-line compared) |
| `get_block` | `[height, hash, commit]` — at least 3, nullable (extra ignored) | `{"Ok": <full block>}` | **IMPLEMENTED / PARTIAL** |
| `get_blocks` | `[start_height, end_height?, max?, include_proof?]` | `{"Ok": {"last_retrieved_height": N, "blocks": […]}}` | **IMPLEMENTED**; `max` capped at 1000 and the range at 10000 heights. Errors `INVALID_PARAMS`, `QUERY_TOO_WIDE`, `NODE_BUSY_RETRY`, `INTERNAL_ERROR`. |
| `get_version` | none | `{"Ok": {"node_version": "Grin++ 1.2.9", "block_header_version": N}}` | **IMPLEMENTED** |
| `get_tip` | none | `{"Ok": {height,last_block_pushed,prev_block_to_last,total_difficulty}}` | **IMPLEMENTED** |
| `get_kernel` | `[excess, min_height?, max_height?]` | `{"Ok": {height, mmr_index, tx_kernel:{features,fee_shift,fee,lock_height,excess,excess_sig}}}` | **IMPLEMENTED**; uses the persistent KernelIndex when it matches the tip, otherwise a bounded direct scan. Errors `NOT_FOUND`, `INTERNAL_ERROR`. |
| `get_outputs` | `[[commits], start_height?, end_height?, include_proof?, include_merkle_proof?]` | `{"Ok": [<output>…]}` | **IMPLEMENTED**; at most 128 commitments and 100 heights per call. `merkle_proof` is always `null` in this slice. |
| `get_unspent_outputs` | `[start_index, end_index?, max?, include_proof?]` | `{"Ok": {"last_retrieved_index": N, "highest_index": M, "outputs": […]}}` | **IMPLEMENTED**; `start_index` is 1-based, `end_index` is a 0-based exclusive PMMR bound, at most 8192 scanned positions and 10000 returned outputs. |
| `get_pmmr_indices` | `[start_block_height, end_block_height?]` | `{"Ok": {"last_retrieved_index": N, "highest_index": M, "outputs": []}}` | **IMPLEMENTED**; maps a height range to output-MMR indices. |
| `get_pool_size` | none | `{"Ok": n}` | **IMPLEMENTED** |
| `get_stempool_size` | none | `{"Ok": n}` | **IMPLEMENTED** |
| `get_unconfirmed_transactions` | none | `{"Ok": [{"src": "Broadcast"\|"Stempool", "tx": {…}}]}` | **IMPLEMENTED** |
| `push_transaction` | `[tx, fluff?]` | `{"Ok": null}` | **IMPLEMENTED / PARTIAL**; transaction validation and pool acceptance errors are not fully mapped to Rust error codes. |
| `get_config` | none | config object (PRIVATE) | **C++ extension**; not in Rust 5.5.2 foreign API |

Every method with missing/incorrect parameters returns `PARAMS_MISSING` or
`INVALID_PARAMS`; unknown methods return `METHOD_NOT_FOUND` (`-32601`).

Example:

```json
{"jsonrpc":"2.0","method":"get_tip","params":[],"id":1}
→ {"jsonrpc":"2.0","result":{"Ok":{"height":3324082,"last_block_pushed":"…","prev_block_to_last":"…","total_difficulty":0}},"id":1}
```

## Owner JSON-RPC (`POST /v2/owner`)

| Method | Params | Result | Access | Status |
|---|---|---|---|---|
| `get_status` | none | `{"Ok": <Rust Status object>}` | PRIVATE | **IMPLEMENTED**; same builder as `/v1/status.rust` |
| `get_peers` | `[address?]` | `{"Ok": [<PeerData>…]}` | PRIVATE | **IMPLEMENTED**; the port in an address is ignored (IP-only identity) |
| `get_connected_peers` | none | `{"Ok": [<PeerData>…]}` | PRIVATE | **IMPLEMENTED**; connected-only fields |
| `ban_peer` | `[address]` | `{"Ok": null}` | ADMIN | **IMPLEMENTED** |
| `unban_peer` | `[address]` | `{"Ok": null}` | ADMIN | **IMPLEMENTED**; also removes DB-only peers |
| `get_config` | none | config object | PRIVATE | **C++ extension** |
| `update_config` | config object | `{"restart_required": false}` | ADMIN | **C++ extension**; validate → persist → apply; a failure leaves the running config unchanged |
| `validate_chain` | — | — | ADMIN | **NOT IMPLEMENTED** — not registered; returns `METHOD_NOT_FOUND`. No backend contract exists. |
| `compact_chain` | — | — | ADMIN | **NOT IMPLEMENTED** — not registered. |
| `reset_chain_head` | — | — | ADMIN | **NOT IMPLEMENTED** — not registered. |
| `invalidate_header` | — | — | ADMIN | **NOT IMPLEMENTED** — not registered. |

`update_config` accepts the keys `min_peers`, `max_peers`,
`min_confirmations`, `reuse_address` (`0`/`1`), `preferred_peers`,
`allowed_peers` and `blocked_peers`. Peer lists are capped at 256 entries.
`min_peers` must be at least the configured `min_sync_peers` and not greater
than `max_peers`.

## Status schema and known Rust differences

`/v1/status` keeps the legacy Grin++ fields and adds the Rust-compatible object
under `rust` plus top-level aliases `tip`, `connections`, `chain_type` and
`sync_status_rust`. One snapshot builder (`NodeStatus::Build`) feeds both
`/v1/status.rust` and owner `get_status`, so the two cannot drift.

Known differences from Rust 5.5.2 (documented as **PARTIAL/OPEN**, deliberately
not hidden):

1. **PIBD phase.** During `SYNCING_TXHASHSET` the C++ node reports
   `sync_status: "txhashset_download"` with the legacy ZIP byte counters
   (`state.downloaded`, `state.download_size`, `network.*`). Rust reports
   `txhashsetpibd_download` with `aborted`, `errored`, `completed_leaves`,
   `leaves_required`, `completed_to_height`, `required_height`. The C++
   `pibd.received_segments` / `required_segments` are PIBD segment counters, not
   leaves, and must not be renamed to Rust leaf fields.
2. **Validation phase.** During `PROCESSING_TXHASHSET` the C++ node has one
   coarse `processing_status` and maps it to the Rust name
   `txhashset_rangeproofs_validation`. Rust separates setup, range proofs
   (`rproofs`, `rproofs_total`), kernels (`kernels`, `kernels_total`), save and
   done. Do not claim an exact Rust schema for these stages.
3. **Peer identity.** C++ peers are keyed by IP; ports are parsed but not part of
   identity. Rust owner methods accept a `SocketAddr`. `get_peers`/`ban_peer`
   therefore behave differently for two peers on one IP with different ports.
4. **REST schema changes.** `GET /v1/peers/<ip>` now returns an object, and
   `lastkernels`/`lastoutputs`/`lastrangeproofs` return `{hash: …}` objects.
   External consumers were not fully re-verified after this intentional change.
5. **Owner chain control.** `validate_chain`, `compact_chain`, `reset_chain_head`
   and `invalidate_header` are missing. They are not stubbed with fake success.
6. **Extensions.** `get_config` (foreign and owner) and `update_config` (owner)
   are C++ extensions. They are not Rust 5.5.2 parity.

## Availability and startup phases

- `/v1/health` is registered before the chain opens and answers `starting`, then
  `ready` once the REST/RPC handlers are created. On the profiled state, `ready`
  preceded `fully_synced` by roughly 19–25 s.
- `/v1/status` is registered with the full REST server, so a 404 in the first
  moments is expected.
- One observation on an earlier binary had `/v1/status` exceed 5 s after state
  validation while `/v1/health` stayed available. The cause was not established;
  treat status latency under load as **PARTIAL/NOT VERIFIED**.

## Verification status of this document

- Handler inventory and access levels were re-checked against the candidate
  code.
- A 27-check local HTTP/RPC matrix (status consistency, empty arrays, invalid
  numbers, owner auth, resync, shutdown, IPv6 ban/unban) passed on intermediate
  candidates (`C996B748…`, `36AD1C92…`, `D748B90E…`). It is **not** re-run on the
  final `4039086C…` binary in this snapshot; per-method parity with Rust is
  **PARTIAL/OPEN**.
- Mainnet API behaviour is **NOT VERIFIED**.

---

[Русская версия](API.ru.md) · [Deutsche Fassung](API.de.md)
