# Architektur — der Node dieses Forks

Diese Seite beschreibt das Node-Modul (`GrinNode`) im Baum
`node-beta.1-candidate`. Wallet und UI liegen außerhalb. Die Beschreibung basiert
auf den Quellen dieses Tags und den erfassten Läufen; nicht verifiziertes
Verhalten ist markiert.

## 1. Komponentenkarte

| Schicht | Hauptkomponenten | Quellen |
|---|---|---|
| Einstiegspunkt | Optionsparsing, Config-Laden, Logging, Hauptschleife, geordnetes Herunterfahren | `src/Server/Main.cpp`, `src/Server/opt.h` |
| Node-Zusammenbau | früher Health-Server, Öffnen von Chain/P2P, vollständiger REST/RPC | `src/Server/Node/Node.cpp`, `NodeRestServer.cpp` |
| REST-API | `/v1`: Status, Chain, Header, Blocks, Peers, TxHashSet, Shutdown, Resync | `src/Server/Node/API/*.cpp` |
| JSON-RPC-API | `/v2/foreign`, `/v2/owner`, Zugriffsstufen, Secret | `src/API/Node/*`, `src/Net/Servers/RPC/*` |
| P2P | Verbindungen, Handshake, Seeding, Peer-Manager, Nachrichtenverarbeitung | `src/P2P/*`, `include/P2P/*` |
| Sync | Header-Sync, State-Sync, Block-Sync, Status-State-Machine | `src/P2P/Sync/*` |
| PIBD | Download, Segment-Service, Scheduler, Session, Aktivierung, optionale Archivauslieferung | `src/P2P/PIBD/*` |
| Chain | State-Machine, Prozessoren, Validatoren, Reorg | `src/BlockChain/*`, `src/BlockChain/Processors/*` |
| Speicher | RocksDB Block-DB, Chain-Dateien/Checkpoints, Header-MMR, TxHashSet/PMMR | `src/Database/*`, `src/BlockChain/ChainState.cpp`, `src/PMMR/*` |
| Validierung | Header-/PoW-/Transaction-/Range-Proof-Prüfung, Worker-Auswahl | `src/BlockChain/Validators/*`, `src/PoW/*`, `src/PMMR/RangeProofBatchVerifier.*` |
| Transaktionspool | Mempool und Stem-Pool | `src/TxPool/*` |

Das Wallet wird aus demselben Baum gebaut, gehört aber nicht zur Node-Beta-
Abnahme; sein Pfad ist nur durch Unit-Tests abgedeckt.

## 2. Start und Neustart

Die Reihenfolge ist bewusst so gewählt: Der Prozess soll beobachtbar sein, während
eine mehrere GB große Datenbank geöffnet wird.

1. Optionen parsen und `server_config.json` (oder Defaults) laden. Daten- und
   Logverzeichnisse anlegen.
2. Frühen HTTP-Listener mit `GET /v1/health` starten, der
   `{"status":"starting"}` liefert. Auf dem Profil kam die erste 200 nach etwa
   `0,56–0,62 s`.
3. Chain öffnen: Chain-Dateien, Header-MMR, RocksDB Block-DB, TxHashSet-Manager.
   KernelIndex prüfen oder neu bauen, wenn er fehlt, an einen anderen Tip gebunden
   ist oder außerhalb der vertrauenswürdigen Erweiterungsgrenzen liegt.
4. Vollen REST/RPC-Server anlegen (alle `/v1`, `/v2/foreign`, `/v2/owner`) und
   Health auf `ready` setzen. `/v1/status` existiert erst ab diesem Punkt.
5. P2P und die Sync-State-Machine starten. `ready` bedeutet nicht
   `fully_synced`: Auf dem Profil lag `ready` etwa 19–25 s vor der vollständigen
   Synchronisierung.

Ein Neustart auf konsistentem Zustand öffnet die Dateien, prüft den Kernel-Index
gegen den Tip und setzt fort. Ein Neustart auf inkonsistentem oder teilweise
geschriebenem Zustand soll aus dem letzten atomaren Checkpoint wiederherstellen
oder fail-closed scheitern, statt einen Teilzustand zu veröffentlichen.
Stromausfall und echter Speicherfehler sind **NOT VERIFIED**; das Kill-Harness
deckt nur erzwungene Prozessbeendigung ab.

## 3. Erstsynchronisierung und State-Machine

`SyncStatus`/`StateSyncer` steuern die Phasen:

1. **Peers.** DNS-Seeds und Peer-Book erzeugen ausgehende Verbindungen; der
   Seeder strebt Min/Max-Peers an und toleriert einzelne Peer-Ausfälle. Ein
   langsamer oder ausgefallener Peer wird ohne Ban rotiert. Header-Anfragen sind
   auf eine pro 200 ms begrenzt.
2. **Header.** Header-Sync treibt die Header-Chain und den Header-MMR zur
   Netzwerkhöhe. `SYNCING_HEADERS` liefert aktuelle/höchste Höhe.
3. **Zustand.** Ist der Node weit zurück, braucht er einen State-Snapshot
   (TxHashSet). Der bevorzugte Pfad ist **PIBD**. Gibt es keinen fähigen Peer oder
   kann PIBD nicht fortgesetzt werden, fällt der Node auf den Legacy-
   `txhashset.zip`-Pfad zurück (`SYNCING_TXHASHSET`, Byte-Zähler).
4. **Prüfung und Aktivierung.** Der geladene Zustand wird geprüft (Kernel-
   Historie, Kernel-Summen, Range Proofs, Kernel-Signaturen) und dann installiert.
   Für den installierten Tip wird der KernelIndex gebaut.
5. **Aufholen.** `SYNCING_BLOCKS` fordert die restlichen Blöcke an, wendet sie an
   und verifiziert den resultierenden Tip.
6. **Vollständig synchron.** `NOT_SYNCING` bedeutet, die Blockhöhe hat das Netz
   erreicht; das Legacy-Feld in `/v1/status` zeigt `FULLY_SYNCED`.

Der beobachtete Live-Floonet-Lauf auf dem kombinierten Build (`16C24DB4…`) folgte
genau dieser Reihenfolge: Header `0 → 3 324 027`, PIBD `4998/4998` Segmente,
Zustandsprüfung etwa `1987 s`, KernelIndex-Generation 2 mit `3 356 610` Kernels,
Aufholen bis `3 324 064`, Endhöhe `3 324 082` nach weiteren Blöcken.

## 4. PIBD: Segmente, Peers, Resume und Recovery

PIBD ersetzt das monolithische ZIP durch unabhängige MMR-Segmente und folgt dem
Wire-Format von Rust Grin 5.5.2.

- **Segmenthöhen** (`PibdParams.h`): Bitmap `2^9` Chunks, Output/Range-Proof/
  Kernel `2^11` Leaves. Anfragen sind begrenzt (`SEGMENT_REQUEST_COUNT = 15` in
  Flight, `20 s` Timeout, kleiner Cache).
- **Download.** `PibdDownload`/`PibdSegmentService` fordern fehlende Segmente von
  fähigen Peers an, prüfen jede Payload gegen erwartete Roots/Größen und bauen
  Output-/Range-Proof-/Kernel-/Bitmap-MMRs in einem Staging-Verzeichnis.
- **Scheduler.** `PibdScheduler` weist jede Anfrage dem am wenigsten belasteten
  Peer innerhalb globaler und Per-Peer-Limits zu. Sendefehler geben die Zuweisung
  frei; Disconnects geben die Zuweisungen des Peers frei; eine Antwort von einem
  unerwarteten Peer wird ohne Segmentverlust abgelehnt; validierte späte Antworten
  werden akzeptiert, wenn keine Zuweisung mehr existiert; Duplikate gelten als
  veraltet. Die Politik ist durch eine deterministische Testmatrix abgedeckt.
  Live-feindliches Verhalten auf P2P-Ban-Ebene ist **NOT VERIFIED**.
- **Resume.** Fortschritt wird auf Platte persistiert; ein Neustart kann mit den
  letzten gültigen Segmenten fortsetzen. Die genaue Matrix je Stufe ist beim
  Kandidaten **PARTIAL/NOT VERIFIED**.
- **Aktivierung.** Ein vollständiger Staging-Zustand trägt einen Completion-
  Marker und wird mit zwei Renames plus Markerdatei
  (`pibd_activation.pending`) eingesetzt. Stirbt der Prozess während des Swaps,
  vollendet der nächste Start die Aktivierung oder stellt das Backup wieder her;
  Live-Dateien werden nie an Ort und Stelle abgeschnitten. Eine fehlgeschlagene
  Recovery wird über `pibd_state.recovery_failed` an den Horizon-Hash gebunden,
  damit der Node keine unmögliche Recovery endlos wiederholt.
- **Legacy-Fallback.** Ist PIBD nicht nutzbar, bleibt `txhashset.zip` mit
  Staging-Extraktion und atomarem Swap. Dieser Pfad ist getrennt und hat seine
  eigene Evidenzhistorie.
- **Archivauslieferung** ist ein Opt-in-Diagnosemodus
  (`GRINPP_PIBD_ARCHIVE_SERVING=1`), kein Teil des Normalbetriebs; öffentliche
  PIBD-Auslieferung ist NOT VERIFIED.

## 5. KernelIndex-Lebenszyklus

`KernelIndex` bildet ein Kernel-Excess-Commitment auf PMMR-Position und
Blockhöhe ab, damit `get_kernel` und die Validierung nicht den ganzen Kernel-MMR
scannen.

- Der Index ist **an eine Generation und einen konkreten Tip gebunden**: Tip-
  Hash/Höhe, Kernel-Root, Kernel-Count und Entry-Count. Ein Lookup wird nur
  genutzt, wenn alle gebundenen Werte zum aktuellen Tip passen.
- Bei der Blockanwendung wird der Index in derselben Chain-Write-Transaktion wie
  der Block aktualisiert: neue Kernel-Positionen werden angehängt, Reorgs rollen
  die betroffenen Positionen zurück. Schlägt die Append-/Rewind-Transaktion fehl,
  bleibt der alte Zustand erhalten und der Chain-Write scheitert fail-closed —
  kein Teilindex wird veröffentlicht.
- Beim Start prüft `Ensure` den persistierten Zustand. Ist der gespeicherte Tip
  ein verifizierter Vorfahr innerhalb der Grenzen (aktuell 4096 Höhen / 32768
  Kernels), werden nur fehlende Kernel in einer Transaktion ergänzt. Sonst läuft
  ein vollständiger Rebuild und veröffentlicht eine neue Generation.
- Orphan-Blöcke werden nach dem Commit repariert (`EnsureKernelIndexLocked`),
  damit ein gültiger Orphan die Chain nicht mit veraltetem Index voranbringt.
- Ein Rebuild mit ~3,36 Mio. Kernels maß `101 870 ms` für Entries, `16 435 ms`
  Commit und `3 833 ms` Read-Back; die Spitze des Working Set erreichte
  `5 715 MB`. Der Chunked-Rebuild zur Senkung der Spitze ist **PLANNED**, nicht
  implementiert.
- Fault Injection an der Index-Transaktionsgrenze und erzwungene Kills sind durch
  Tests abgedeckt. Echter RocksDB-Fehler, Post-Commit und Stromausfall sind
  **NOT VERIFIED**.

## 6. Validierung und Worker-Auswahl

- Header werden auf Chain-Verkettung, PoW, Header-MMR-Leaves und -Innenknoten,
  Timestamp, Version und Total Difficulty geprüft. Header-/MMR-Validierung dauert
  `57,8–88,0 s` auf dem Profil mit ~3,32 Mio. Headern; eine integritätserhaltende
  parallele Chunk-Beschleunigung ist **PLANNED**.
- Range Proofs werden in Batches geprüft (`RangeProofBatchVerifier`). Default ist
  **1** Worker. `GRINPP_RANGEPROOF_WORKERS=auto` aktiviert eine begrenzte
  automatische Auswahl anhand Prozess-Affinität, physischer Kerne, verfügbarem
  Speicher und Windows-Job-Object-Limits; `…=1|2|3|4|…` erzwingt eine Zahl.
- Die Job-Object-Buchhaltung nutzt Per-Process- und Job-weite committed memory,
  nimmt das Minimum des Restlimits, behandelt `< 1 MiB` exakt und fällt bei
  unbekanntem Limit konservativ auf einen Worker zurück. Ein Vollzustands-
  Benchmark 1/2/3/4/auto ist **OPEN**; Default bleibt 1.
- Ein lokaler Fehler bei der Blockverarbeitung (z. B. ein Index-Schreibfehler)
  darf keinen Peer bannen: Die Peer-Validität ist unbekannt. Das ist durch den
  BlockPipe-Vertragstest abgedeckt.

## 7. FastInit Header-Checkpoint (experimentell)

`HeaderStateCheckpoint` ist eine **experimentelle Opt-in**-Beschleunigung der
Header-Prüfung. Kein Teil des normalen Starts und nicht als Produktion
darzustellen.

Vertrauensmodell in der aktuellen Implementierung:

1. Der Checkpoint enthält einen vollständigen Header-Chain-Zustand und wird nur
   nach vollständiger Prüfung akzeptiert (Leaf- und innere MMR-Knoten, Sequenz,
   Version, Timestamp, Previous Root, Difficulty-Monotonie, Confirmed-Chain-
   Präfix, Chain-/MMR-Größen).
2. Ein **erster** Checkpoint wird nur geschrieben, wenn explizit
   `GRINPP_HEADER_CKPT_BOOTSTRAP=1` gesetzt ist **und** ein veröffentlichter
   Aggregate-Anchor über `GRINPP_HEADER_CKPT_TRUST_ANCHOR` vorliegt. Ohne Anchor
   loggt der Node `header_checkpoint_not_written reason=no_trusted_anchor` und
   prüft weiter vollständig.
3. Ein vorhandener Checkpoint wird nur bei passenden Metadaten zu den Chain-
   Dateien wiederverwendet; sonst Fallback auf Vollprüfung.
4. Nach erfolgreichem Sync kann der Checkpoint aktualisiert werden. Der gemessene
   Refresh auf dem Profil war `9,56 s`; ein normaler Neustart mit Checkpoint
   `7,8 s`.
5. Wird der Prozess während eines Checkpoint-Refreshs getötet, bleibt der alte
   Checkpoint intakt (einmal beobachtet: 31 081 Bytes, Hash unverändert).

Grenzen: Ein echter Reorg auf einem gültigen alternativen Branch, Profil-Lock-
Verhalten und Post-Kill-Fortsetzung sind nicht verifiziert. Die Zeitangaben sind
keine finale kontrollierte Serie. Jedes FastInit-Ergebnis als **EXPERIMENTAL /
NOT VERIFIED** behandeln.

## 8. On-Disk-Layout

Standardwurzel: `%USERPROFILE%\.GrinPP\<NETWORK>` (überschreibbar mit `DATA_PATH`
in der Config). Alles unten liegt unter dieser Wurzel.

| Pfad | Inhalt |
|---|---|
| `server_config.json` | Node-Konfiguration (sofern kein `--config-path` genutzt wird) |
| `.api_secret` | 64-Hex-API-Secret, owner-only; beim ersten Start erzeugt |
| `LOGS\` | Node-Logs |
| `NODE\CHAIN\` | `confirmed.chain`, `candidate.chain`, `header_mmr.bin`, `chain.checkpoint` (atomarer Checkpoint), optional `header.ckpt` (FastInit) |
| `NODE\DB\` | RocksDB Block-/Kernel-DB |
| `NODE\TXHASHSET\` | Live-Zustand: `output/`, `rangeproof/`, `kernel/`-MMRs, `txhashset.bin` und zugehörige Dateien |
| `NODE\pibd_staging\` | Laufender PIBD-Zustand (gleiches Layout) |
| `NODE\pibd_activation.pending` | Marker eines laufenden Aktivierungs-Swaps |
| `NODE\TXHASHSET\pibd_state.complete` | Completion-Marker in einem geprüften Staging-/Live-Zustand |
| `NODE\TXHASHSET\pibd_state.recovery_failed` | Gebundener Marker gegen endlose Recovery eines Horizonts |
| `TOR\` | Tor-Daten bei aktiviertem Tor |
| `logs\` neben der EXE | Diagnose-Logs; Fallback auf `LOGS\node-diagnostics`, wenn nicht beschreibbar |

Profile sind nicht ohne Weiteres zwischen Maschinen portierbar: DB-Lock, ACL des
API-Secrets und absolute Pfade in der Config zählen. Kopieren Sie niemals ein
Profil mit echten Wallet-Schlüsseln in eine Testumgebung.

## 9. API und Vertrauensgrenzen

- Der HTTP-Server lauscht lokal. PRIVATE/ADMIN verlangen API-Secret und Loopback-
  `Host`/`Origin`; PUBLIC ist read-only. Siehe [API.de.md](API.de.md).
- Peer-Eingaben sind nicht vertrauenswürdig: Nachrichtenparser haben explizite
  Limits, fehlerhafte oder root-fremde Segmente scheitern fail-closed, und ein
  Peer wird nicht für einen lokalen Verarbeitungsfehler gebannt.
- Der Node loggt nie das API-Secret, Seed-Wörter oder private Schlüssel. Fügen Sie
  sie nicht zur Diagnose hinzu.

## 10. Bekannte Architekturgrenzen

- Kein verifizierter Mainnet-Lauf; keine Verifikation von Stromausfall oder
  echtem Datenträgerfehler.
- Öffentlicher Reorg nur synthetisch; echter Netzwerk-Reorg ist NOT VERIFIED.
- Vollzustands-Worker-Benchmark und automatische Default-Wahl sind offen.
- KernelIndex-Rebuild-Spitze (~5,7 GB) und Header-Validierungszeit (~58–88 s)
  sind ungelöste Performance-Punkte.
- FastInit und PIHD sind experimentell/geplant und standardmäßig aus.
- Wallet und UI sind getrennte, nicht verifizierte Strecken.

---

[English version](ARCHITECTURE.md) · [Русская версия](ARCHITECTURE.ru.md)
