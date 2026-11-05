# HTTP- und JSON-RPC-API des Nodes

Dieses Dokument beschreibt die Node-API des Baums `node-beta.1-candidate`. Quelle
der Wahrheit ist der Code:

- REST `/v1`: `src/Server/Node/NodeRestServer.cpp` und
  `src/Server/Node/API/*.cpp`.
- JSON-RPC-Registrierung: `src/API/Node/NodeServer.cpp`,
  `src/API/Node/Handlers/*.h`.
- Zugriffskontrolle: `include/Net/Servers/RPC/ApiAccess.h`,
  `src/Net/Servers/RPC/ApiAccess.cpp`, `include/Net/Servers/RPC/RPCServer.h`.

Status:

- **IMPLEMENTED** — Handler existiert und ist durch Tests und/oder lokale
  HTTP-Prüfungen abgedeckt.
- **PARTIAL** — implementiert, aber Schema, Fehlermatrix, Rechte oder Live-Phasen
  sind nicht vollständig gegen die Referenz geprüft.
- **NOT IMPLEMENTED** — kein Handler registriert; kein vorgetäuschter Erfolg.

Referenz für den Vergleich ist **Rust Grin 5.5.2** (`reference/grin-5.5.2`,
`api/src/foreign_rpc.rs`, `api/src/owner_rpc.rs`). Der Fork ergänzt bewusst
C++-Erweiterungen; ein Unterschied ist nicht automatisch ein Fehler, muss aber
für Verbraucher dokumentiert sein.

## Server und Authentifizierung

REST und JSON-RPC laufen auf einem lokalen Listener an `SERVER.REST_API_PORT`
(Standard `3413` Mainnet, `13413` Floonet). JSON-RPC-Endpunkte sind
`POST /v2/foreign` und `POST /v2/owner`; der Body ist JSON-RPC 2.0
(`{"jsonrpc":"2.0","method":"…","params":[…],"id":1}`). REST antwortet mit
normalem JSON (kein JSON-RPC).

Beim ersten Start erzeugt der Node ein 64-Hex-Secret unter
`<DATA_PATH>/.api_secret`. Es wird nie geloggt und nie in URLs gesetzt. Windows:
Datei mit geschütztem Owner-Only-DACL (Besitzer + SYSTEM); POSIX: Modus `0600`.
Ein fehlendes, fehlerhaftes oder unsicheres Secret führt für PRIVATE/ADMIN zu
Fail-Closed; der Node ersetzt es nicht still.

Header (einer genügt):

```
Authorization: Bearer <secret>
X-API-Secret: <secret>
```

Der `Host` muss Loopback sein (`127.0.0.1`, `localhost`, `::1`); ein vorhandener
`Origin` muss Loopback oder `null` sein. Die Bindung an `EServerType::LOCAL` ist
für sich genommen keine Authentifizierung.

| Stufe | Wofür | Fehler |
|---|---|---|
| PUBLIC | Read-only Chain-/Peer-Daten, Health | kein Secret nötig |
| PRIVATE | Status/Peers/Config lesen | JSON-RPC `-32001`, REST `401` |
| ADMIN | Config schreiben, Peer-Ban/Unban, Resync, Shutdown | JSON-RPC `-32001`, REST `401` |

ADMIN: `ban_peer`/`unban_peer`, `update_config`, `/v1/resync`, REST-Ban/Unban,
`/v1/shutdown`. PRIVATE: `get_status`, `get_peers`, `get_connected_peers`,
`get_config`. Foreign-Read-Methoden sind PUBLIC.

## REST `/v1`

`GET /v1/` liefert eine JSON-Liste der Routen. Mehrere Read-Handler erzwingen
die HTTP-Methode nicht selbst; verwenden Sie die dokumentierte Methode und bauen
Sie keine Clients auf undefiniertem Verhalten.

| Methode und Pfad | Zweck | Status | Hinweise / Fehler |
|---|---|---|---|
| `GET /v1/health` | API-/Prozessbereitschaft | **IMPLEMENTED** | `{"status":"starting"\|"ready"}`. Vor der Chain verfügbar, erste 200 meist nach `0,56–0,62 s`. `ready` heißt Handler registriert, nicht synchronisiert. Nicht-GET → 404. |
| `GET /v1/status` | Node-/Chain-/Sync-Zustand | **PARTIAL** | Legacy-Felder plus Rust-kompatibles `rust`-Objekt; 500 ohne Tip. Kann in den ersten Millisekunden 404 sein und überschritt einmal 5 s nach der Zustandsprüfung; Ursache nicht festgestellt. |
| `GET\|POST /v1/resync` | Chain-Resync und Unban aller Peers | **IMPLEMENTED** | ADMIN. Seiteneffekte: `ResyncChain` + `UnbanAllPeers`; 401 ohne Secret. |
| `GET /v1/headers/<hash\|height\|commit>` | Ein Header | **IMPLEMENTED** | 64-Hex = Hash, 66-Hex = Commitment, sonst Dezimalhöhe. 400 `HEADER NOT FOUND`. |
| `GET /v1/blocks/<hash\|height\|commit>[?compact]` | Voller oder kompakter Block | **IMPLEMENTED** | 400 `BLOCK NOT FOUND`; `?compact` liefert kompakten Block. |
| `GET /v1/chain` | Bestätigter Tip | **IMPLEMENTED** | `{height,last_block_pushed,prev_block_to_last,total_difficulty}`; 500 ohne Tip. |
| `GET /v1/chain/outputs/byids?id=<commit>[,<commit>]…` | Output-Positionen nach Commitment | **IMPLEMENTED** | Array `{commit,height,mmr_index}`; unbekannte IDs werden ausgelassen. 400 bei Parsefehler. |
| `GET /v1/chain/outputs/byheight?start_height=N&end_height=M` | Blöcke mit Outputs im Höhenbereich | **IMPLEMENTED** | `end_height` fällt auf `start_height` zurück, wenn es fehlt oder kleiner ist. 400 bei ungültigen Zahlen. |
| `GET /v1/peers/all` | Bekannte Peers | **IMPLEMENTED** | Array von Peer-Objekten. |
| `GET /v1/peers/connected` | Verbundene Peers | **IMPLEMENTED** | Array von Connected-Peer-Objekten. |
| `GET /v1/peers/diagnostics` | Aggregierte Peer-/Download-Diagnose | **IMPLEMENTED** | `totals`, `banned`, `connected`, `download`, `versions`. Adressen sind Betriebsdaten; bei Veröffentlichung als sensibel behandeln. |
| `GET /v1/peers/<ip>` | Ein bekannter Peer | **IMPLEMENTED** | Bei neueren Kandidaten ein **Objekt** (älterer Vertrag: Array mit einem Element). 404 `Peer not found.` |
| `POST /v1/peers/<ip>/ban` | Manueller Ban | **IMPLEMENTED** | ADMIN; leerer Erfolgsbody. Port wird geparst, Peer-Identität ist aber nur die IP. |
| `POST /v1/peers/<ip>/unban` | Manueller Unban | **IMPLEMENTED** | ADMIN; umfasst Nur-DB-Peers. |
| `GET /v1/txhashset/roots` | Output-/Range-Proof-/Kernel-Roots des Tips | **IMPLEMENTED** | 404 ohne Tip. |
| `GET /v1/txhashset/lastkernels?n=N` | Letzte N Kernel-Hashes | **IMPLEMENTED** | Array `{"hash":"…"}`; Default 10, max. 1000. Ältere Builds lieferten Strings. 400, wenn Query nicht mit `n=` beginnt; 500 ohne TxHashSet. |
| `GET /v1/txhashset/lastoutputs?n=N` | Letzte N Output-Hashes | **IMPLEMENTED** | Array `{hash}`; gleiche Limits. |
| `GET /v1/txhashset/lastrangeproofs?n=N` | Letzte N Range-Proof-Hashes | **IMPLEMENTED** | Array `{hash}`; gleiche Limits. |
| `GET /v1/txhashset/outputs?start_index=N&max=M` | Seite unverbrauchter Outputs | **IMPLEMENTED** | `{highest_index,last_retrieved_index,outputs[]}`; `max` auf 1000 begrenzt. |
| `POST /v1/shutdown` | Geordnetes Herunterfahren | **IMPLEMENTED** | ADMIN; Nicht-POST → 404. Leerer Body bei Erfolg. |

### Beispiel `/v1/status` (gekürzt)

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

`pibd` (`received_segments`, `required_segments`) erscheint nur, nachdem PIBD
gestartet ist. `sync_info` erscheint nur bei echten Phasendaten.

## Foreign JSON-RPC (`POST /v2/foreign`)

Alle 13 Methodennamen aus Rust Grin 5.5.2 sind registriert. Registrierung ist
keine Schema-Parität; die Prüfung von Parametern, Ergebnissen und Fehlern pro
Methode ist **PARTIAL**, sofern nicht anders angegeben.

| Methode | Parameter | Ergebnis | Status |
|---|---|---|---|
| `get_header` | `[height, hash, commit]` — mindestens 3, nullable (Extras ignoriert) | `{"Ok": <header>}` | **IMPLEMENTED / PARTIAL** (Schema nicht Zeile für Zeile verglichen) |
| `get_block` | `[height, hash, commit]` — mindestens 3, nullable (Extras ignoriert) | `{"Ok": <full block>}` | **IMPLEMENTED / PARTIAL** |
| `get_blocks` | `[start_height, end_height?, max?, include_proof?]` | `{"Ok": {"last_retrieved_height": N, "blocks": […]}}` | **IMPLEMENTED**; `max` ≤ 1000, Bereich ≤ 10000 Höhen. Fehler `INVALID_PARAMS`, `QUERY_TOO_WIDE`, `NODE_BUSY_RETRY`, `INTERNAL_ERROR`. |
| `get_version` | keine | `{"Ok": {"node_version": "Grin++ 1.2.9", "block_header_version": N}}` | **IMPLEMENTED** |
| `get_tip` | keine | `{"Ok": {height,last_block_pushed,prev_block_to_last,total_difficulty}}` | **IMPLEMENTED** |
| `get_kernel` | `[excess, min_height?, max_height?]` | `{"Ok": {height, mmr_index, tx_kernel:{features,fee_shift,fee,lock_height,excess,excess_sig}}}` | **IMPLEMENTED**; nutzt den persistenten KernelIndex bei Tip-Übereinstimmung, sonst begrenzten Direct-Scan. Fehler `NOT_FOUND`, `INTERNAL_ERROR`. |
| `get_outputs` | `[[commits], start_height?, end_height?, include_proof?, include_merkle_proof?]` | `{"Ok": [<output>…]}` | **IMPLEMENTED**; max. 128 Commitments und 100 Höhen pro Aufruf. `merkle_proof` ist immer `null` in diesem Schnitt. |
| `get_unspent_outputs` | `[start_index, end_index?, max?, include_proof?]` | `{"Ok": {"last_retrieved_index": N, "highest_index": M, "outputs": […]}}` | **IMPLEMENTED**; `start_index` 1-basiert, `end_index` 0-basierte exklusive PMMR-Grenze, max. 8192 Positionen und 10000 Outputs. |
| `get_pmmr_indices` | `[start_block_height, end_block_height?]` | `{"Ok": {"last_retrieved_index": N, "highest_index": M, "outputs": []}}` | **IMPLEMENTED**; bildet einen Höhenbereich auf Output-MMR-Indizes ab. |
| `get_pool_size` | keine | `{"Ok": n}` | **IMPLEMENTED** |
| `get_stempool_size` | keine | `{"Ok": n}` | **IMPLEMENTED** |
| `get_unconfirmed_transactions` | keine | `{"Ok": [{"src": "Broadcast"\|"Stempool", "tx": {…}}]}` | **IMPLEMENTED** |
| `push_transaction` | `[tx, fluff?]` | `{"Ok": null}` | **IMPLEMENTED / PARTIAL**; Validierungs-/Pool-Fehler sind nicht vollständig auf Rust-Codes abgebildet. |
| `get_config` | keine | Config-Objekt (PRIVATE) | **C++-Erweiterung**; nicht in Rust 5.5.2 Foreign API |

Fehlende/falsche Parameter liefern `PARAMS_MISSING`/`INVALID_PARAMS`; unbekannte
Methoden `METHOD_NOT_FOUND` (`-32601`).

Beispiel:

```json
{"jsonrpc":"2.0","method":"get_tip","params":[],"id":1}
→ {"jsonrpc":"2.0","result":{"Ok":{"height":3324082,"last_block_pushed":"…","prev_block_to_last":"…","total_difficulty":0}},"id":1}
```

## Owner JSON-RPC (`POST /v2/owner`)

| Methode | Parameter | Ergebnis | Zugriff | Status |
|---|---|---|---|---|
| `get_status` | keine | `{"Ok": <Rust Status>}` | PRIVATE | **IMPLEMENTED**; gleicher Builder wie `/v1/status.rust` |
| `get_peers` | `[address?]` | `{"Ok": [<PeerData>…]}` | PRIVATE | **IMPLEMENTED**; Port in der Adresse wird ignoriert (nur IP) |
| `get_connected_peers` | keine | `{"Ok": [<PeerData>…]}` | PRIVATE | **IMPLEMENTED**; nur Connected-Felder |
| `ban_peer` | `[address]` | `{"Ok": null}` | ADMIN | **IMPLEMENTED** |
| `unban_peer` | `[address]` | `{"Ok": null}` | ADMIN | **IMPLEMENTED**; entfernt auch Nur-DB-Peers |
| `get_config` | keine | Config-Objekt | PRIVATE | **C++-Erweiterung** |
| `update_config` | Config-Objekt | `{"restart_required": false}` | ADMIN | **C++-Erweiterung**; validate → persist → apply; bei Fehler bleibt die Laufzeit-Config unverändert |
| `validate_chain` | — | — | ADMIN | **NOT IMPLEMENTED** — nicht registriert; `METHOD_NOT_FOUND`. Kein Backend-Vertrag. |
| `compact_chain` | — | — | ADMIN | **NOT IMPLEMENTED** — nicht registriert. |
| `reset_chain_head` | — | — | ADMIN | **NOT IMPLEMENTED** — nicht registriert. |
| `invalidate_header` | — | — | ADMIN | **NOT IMPLEMENTED** — nicht registriert. |

`update_config` akzeptiert `min_peers`, `max_peers`, `min_confirmations`,
`reuse_address` (`0`/`1`), `preferred_peers`, `allowed_peers`, `blocked_peers`.
Peer-Listen sind auf 256 Einträge begrenzt. `min_peers` muss mindestens
`min_sync_peers` und höchstens `max_peers` sein.

## Status-Schema und bekannte Rust-Unterschiede

`/v1/status` behält die Legacy-Felder von Grin++ und ergänzt das
Rust-kompatible Objekt `rust` sowie die Aliase `tip`, `connections`,
`chain_type`, `sync_status_rust`. Ein Snapshot-Builder (`NodeStatus::Build`)
speist `/v1/status.rust` und Owner-`get_status`, daher können sie nicht
auseinanderlaufen.

Bekannte Unterschiede zu Rust 5.5.2 (**PARTIAL/OPEN**, bewusst nicht versteckt):

1. **PIBD-Phase.** Während `SYNCING_TXHASHSET` meldet C++
   `sync_status: "txhashset_download"` mit den Legacy-ZIP-Byte-Zählern
   (`state.downloaded`, `state.download_size`). Rust meldet
   `txhashsetpibd_download` mit `aborted`, `errored`, `completed_leaves`,
   `leaves_required`, `completed_to_height`, `required_height`.
   `pibd.received_segments`/`required_segments` sind Segmentzähler, keine Leaves,
   und dürfen nicht in Rust-Leaf-Felder umbenannt werden.
2. **Validierungsphase.** Während `PROCESSING_TXHASHSET` hat C++ einen groben
   `processing_status`, abgebildet auf den Rust-Namen
   `txhashset_rangeproofs_validation`. Rust trennt Setup, Range Proofs
   (`rproofs`, `rproofs_total`), Kernel (`kernels`, `kernels_total`), Save und
   Done. Für diese Stufen kein exaktes Rust-Schema behaupten.
3. **Peer-Identität.** C++ keyt Peers über die IP; Ports werden geparst, gehören
   aber nicht zur Identität. Rust-Owner akzeptiert `SocketAddr`. Für zwei Peers
   auf einer IP mit verschiedenen Ports weicht das Verhalten ab.
4. **REST-Schemaänderungen.** `GET /v1/peers/<ip>` liefert jetzt ein Objekt,
   `lastkernels`/`lastoutputs`/`lastrangeproofs` liefern `{hash: …}`-Objekte.
   Externe Verbraucher wurden nach dieser bewussten Änderung nicht vollständig
   erneut geprüft.
5. **Owner-Chain-Kontrolle.** `validate_chain`, `compact_chain`,
   `reset_chain_head`, `invalidate_header` fehlen. Es gibt keinen vorgetäuschten
   Erfolg.
6. **Erweiterungen.** `get_config` (Foreign und Owner) und `update_config`
   (Owner) sind C++-Erweiterungen, keine Rust-5.5.2-Parität.

## Verfügbarkeit und Startphasen

- `/v1/health` wird vor dem Öffnen der Chain registriert und antwortet
  `starting`, dann `ready` nach Erstellung der REST/RPC-Handler. Auf dem Profil
  lag `ready` etwa 19–25 s vor `fully_synced`.
- `/v1/status` wird mit dem vollen REST-Server registriert; ein 404 in den ersten
  Momenten ist zu erwarten.
- Eine Beobachtung auf einem früheren Binary: `/v1/status` überschritt nach der
  Zustandsprüfung 5 s, während `/v1/health` verfügbar blieb. Ursache nicht
  festgestellt; Status-Latenz unter Last ist **PARTIAL/NOT VERIFIED**.

## Prüfstatus dieses Dokuments

- Handler-Inventar und Zugriffsstufen wurden gegen den Kandidatencode geprüft.
- Eine Matrix aus 27 lokalen HTTP/RPC-Prüfungen bestand auf Zwischenkandidaten
  (`C996B748…`, `36AD1C92…`, `D748B90E…`). Auf dem finalen `4039086C…` wurde sie
  in diesem Stand **nicht erneut ausgeführt**; Methodenparität zu Rust ist
  **PARTIAL/OPEN**.
- Mainnet-API-Verhalten ist **NOT VERIFIED**.

---

[English version](API.md) · [Русская версия](API.ru.md)
