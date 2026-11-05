# Von Upstream v1.2.9 zum aktuellen Kandidaten

Dieses Dokument erklärt, was sich zwischen Upstream GrinPlusPlus `v1.2.9` und dem
Baum `node-beta.1-candidate` geändert hat, warum, was verifiziert ist und was
offen bleibt. Kanonisch ist Englisch:
[FROM-UPSTREAM-TO-CURRENT.en.md](FROM-UPSTREAM-TO-CURRENT.en.md).

## Herkunft und Rekonstruktion (zuerst lesen)

- Upstream: https://github.com/GrinPlusPlus/GrinPlusPlus, Tag `v1.2.9`, Commit
  `a0958a391c30cf630e3e08534fbc6fe9fb194326` (22.10.2024). Upstream-Historie und
  Autorschaft bleiben erhalten.
- Die Entwicklung 2026 fand in einem lokalen Arbeitsbereich statt, dessen
  Quellbaum mit **einem squashed Import-Commit** begann. Ursprüngliche
  Chronologie, Branchnamen und Zwischencommits sind nicht erhalten.
- Die Commits nach `v1.2.9` sind **REKONSTRUIERT** aus dem verifizierten finalen
  Diff und der Projektevidenz. Sie sind nach Zweck gruppiert, nicht nach
  tatsächlicher Reihenfolge. Rekonstruierte Commits nutzen eine ehrliche
  Fork-Preparation-Identität und Evidenzverweise im Message-Trailer; Upstream-
  Autoren und -Daten werden nicht erfunden.
- „Commit A führte X ein“ bedeutet hier daher „die rekonstruierte Gruppe, der X
  gehört“, keine Aussage über die ursprüngliche Chronologie.
- Exakte Quellidentität: Manifest
  `docs/evidence/files/candidate-source-manifest-final.sha256` (1528 Dateien,
  SHA-256 `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627`).

### Rekonstruierte Commit-Gruppen

Der Tag `node-beta.1-candidate` ist ein annotierter Tag (Objekt
`317d143d2fc0d8b7ca8a7190521bca3e2f211a38`) auf dem Commit
`7efdb02e2a4d714d406b88b5610cb6e1c9d2bf59`. Die kurzen SHAs unten sind die
rekonstruierte Serie; wird der Tag vor der Veröffentlichung verschoben, lesen Sie
`git log v1.2.9..node-beta.1-candidate` neu und aktualisieren Sie die Tabelle in
derselben Änderung.

| Gruppe | Umfang | Commit |
|---|---|---|
| A | Build-/Test-Infrastruktur und Repository-Hygiene | `55b4d114`, `079fce77` |
| B | Härtung von Wire-Format, Locking, Logging und Datei-I/O | `1939ace0` |
| C | Dauerhafte Chain-Stores, Recovery, Checkpoints und Fail-Closed-Rewind | `5cd26f88` |
| D | PIBD-Modelle, -Nachrichten und MMR-Traversierung | `ebe67ed3` |
| E | PIBD-Download, -Prüfung, Resume und atomare Aktivierung | `b0e60e86` |
| F | Peer-Hygiene, Multi-Peer-Versand und PIBD/PIHD-Client | `5d83acef` |
| G | Persistenter KernelIndex und schneller Kernel-Lookup | `fe4ea6c2` |
| H | NRD-Validierung, PoW-Edge-Bits und parallele Range-Proof-Prüfung | `4edc061a` |
| I | Rust-kompatible REST-/Owner-API, Zugriffskontrolle und Status | `b6b39413` |
| J | Experimenteller Opt-in-FastInit-Header-Checkpoint | `67ed50d3` |
| K | Wallet-Node-Client, SQLite-Store und Lifecycle-Fixes | `9c289387` |
| L | Drittanbieter-Hinweise, Abhängigkeitslizenzen, Omitted-Tools | `0f3e3e65` |
| M | Integration: PIBD, KernelIndex und FastInit in den Sync-Pfaden | `7efdb02e` |

## Statusvokabular

**PASS** — beobachtet auf dem genannten Binary/Szenario; **PARTIAL** —
implementiert und teilweise geprüft; **FAIL** — beobachteter Fehler; **NOT
VERIFIED** — keine qualifizierende Evidenz; **PLANNED** — nur Entwurf;
**EXPERIMENTAL** — Opt-in-Code, keine Produktion.

## 1. Wire-Format- und Fehlerbehandlungs-Härtung

- **Ausgangsverhalten (v1.2.9).** Nachrichtenparser konnten bei feindlichen
  Längen überlesen oder überlaufen; mehrere Fehlerpfade warfen generische
  Exceptions; Capability- und Nachrichtentabellen waren gegenüber Grin 5.5.2
  unvollständig.
- **Problem und Bedingungen.** Fehlerhafte/abgeschnittene Peer-Nachrichten, große
  Varints/Puffer, falsche Magic Bytes und unbekannte Typen konnten die
  Verarbeitung abbrechen statt eine Nachricht sauber abzulehnen. Festgestellt
  durch Codeanalyse und Malformed-Input-Tests; einige historische Abstürze wurden
  beobachtet, ihre Ursache aber nicht vollständig geklärt.
- **Untersuchung.** Der Vertrag wurde mit Rust Grin 5.5.2 (`p2p/`, `core/`-
  Serialisierung) verglichen und mit Fuzz-Korpora und Negativfällen geprüft.
- **Änderung.** Begrenztes Lesen und explizite Serialisierungsfehler in
  `ByteBuffer`, Header-/Typ-/Capability-Tabellen, Block-/Compact-Block-/Kernel-
  Parsing und Fail-Closed an der P2P-Nachrichtengrenze; Limits für Header-MMR und
  Blockheader.
- **Komponenten.** `include/Core/Serialization/ByteBuffer.h`,
  `include/P2P/{Capabilities,Common,MessageTypes}.h`, `src/P2P/Messages/*`,
  `src/Core/Models/*`, `src/P2P/MessageProcessor.cpp`.
- **Tests/Ergebnisse.** Unit- und Negativtests sind Teil der 373 Fälle;
  historische ASan-/Fuzz-Kampagnen liegen in der lokalen Evidenz. Eine frische
  Fuzz-/ASan-Kampagne auf dem finalen Kandidaten ist **NOT VERIFIED**.
- **Kompatibilität/Performance/Daten.** Kein Formatwechsel für gültige Peers;
  ungültige Nachrichten schlagen fail-closed fehl. Keine Datenmigration.
- **Rest.** Vollständige Sanitizer-Abdeckung und frischer Fuzz-Lauf offen; einige
  historische Absturzberichte ungeklärt.
- **Links.** [../evidence/INDEX.de.md](../evidence/INDEX.de.md),
  [../API.de.md](../API.de.md).

## 2. Peers, Rate-Limit und Block-Sync-Diagnose

- **Ausgangsverhalten.** Der Node bannte einen Peer wegen „Rate-Limit“ anhand
  eines Zählers, der auch eigene ausgehende Nachrichten enthielt; schneller
  Header-Sync konnte einen korrekten Peer bannen. Block-Sync zählte jedes
  Ausbleiben einer Antwort als Timeout, ohne frühes Cooldown-Reassignment zu
  trennen; manuelles Unban bereinigte keine Nur-DB-Peers.
- **Problem/Bedingungen.** Beobachtet in lokalen und öffentlichen Floonet-Läufen
  (04.11.2026: `Banning 127.0.0.1 for exceeding rate limit` beim Header-
  Download). Header-Anfragen wurden ungebremst gesendet.
- **Untersuchung.** Vergleich mit Rust Grin 5.5.2: Missbrauchszählung nur über
  eingehende Nachrichten; Header-Anfragen getaktet. Zähler und Peer-Book wurden
  gegen die Rust-Sync-Politik geprüft.
- **Änderung.** Rate-Limit zählt nur eingehende Nachrichten; Header-Anfragen
  höchstens alle 200 ms; Timeout weist einem anderen Peer zu, ohne Ban;
  `timeouts`, `retries`, `reassignments`, `cooldown_reassignments` getrennt;
  manuelles Ban/Unban funktioniert für Nur-DB-Peers und rollt bei Schreibfehler
  zurück; Diagnose zeigt Peer- und Download-Zustand.
- **Komponenten.** `src/P2P/Connection.cpp`, `BlockSyncer.*`, `PeerManager.*`,
  `Seeder.*`, `src/Server/Node/API/PeersAPI.cpp`, `src/P2P/SyncStatus.*`.
- **Tests/Ergebnisse.** Fokustests: Rate 5 Assertions, BlockSyncPolicy 35, Unban
  19. Kurzer Live-Floonet-Catch-up bestand (22 Blöcke, `FULLY_SYNCED`); der
  kombinierte Volllauf bestand mit 10 Verbindungen ohne Invalid-Data-/Fraud-Bans.
  Zählerbedeutung auf Zwischenkandidaten geprüft.
- **Kompatibilität/Performance/Daten.** Keine API-Entfernung; klarere Semantik,
  teils neue Namen. Ein Peer kann früher erneut versucht werden.
- **Rest.** Ein-Peer-Recovery, langer Catch-up, feindliche Peers und
  `/v1/status`-Latenz unter Last sind **NOT VERIFIED**. Timeouts können Requests
  übersteigen; das ist kein Netzwerkfehlerbeweis.
- **Links.** [../evidence/INDEX.de.md](../evidence/INDEX.de.md).

## 3. PIBD-State-Sync

- **Ausgangsverhalten.** Der State-Download nutzte den monolithischen
  `txhashset.zip`-Pfad mit Byte-Zählern und langer Prüfung.
- **Problem/Bedingungen.** Der ZIP-Pfad ist langsam, Alles-oder-nichts und kaum
  wiederaufsetzbar; ein Neustart während des Downloads war teuer. Ziel war PIBD
  nach Grin 5.5.2 mit Legacy-Pfad als Fallback.
- **Untersuchung.** Wire-Format, Segmentarithmetik, Bitmap-Chunk-Layout,
  Root-Berechnung und Aktivierungsreihenfolge wurden aus Rust Grin 5.5.2
  übernommen und, wo Fixtures vorhanden waren, mit Oracle-/Vektortests geprüft.
- **Änderung.** Segmentmodelle/-nachrichten, Download und Prüfung, gestaffelte
  Montage der vier MMRs, Validierung, atomare Aktivierung mit Marker-Recovery,
  Resume, Peer-Rotation bei Timeouts/Nullfortschritt und Legacy-ZIP-Fallback.
  Archiv-Auslieferung existiert nur als Opt-in-Diagnosemodus.
- **Komponenten.** `src/P2P/PIBD/*`, `src/P2P/Sync/StateSyncer.*`, `src/PMMR/*`,
  `src/BlockChain/Processors/TxHashSetProcessor.*`,
  `src/P2P/Pipeline/TxHashSetPipe.*`.
- **Tests/Ergebnisse.** Synthetische Archivinstallation + Index + Neustart: 25
  Assertions PASS. Auf dem früheren kombinierten Build lud ein Live-Floonet-Lauf
  `4998/4998` Segmente, validierte den Zustand und erreichte `fully_synced`. Beim
  Kandidaten selbst begann ein frischer isolierter Floonet-Lauf PIBD bei
  `3 321 360`, verifizierte `2 948 812/2 948 812` Range Proofs in `1 215 538 ms`
  mit 1 Worker, validierte das TxHashSet, baute KernelIndex-Generation 2
  (`3 357 330` Kernels), holte auf, startete um 00:53 mit
  `kernel_index_check result=current` ohne Rebuild neu und erreichte
  `FULLY_SYNCED` bei `3 324 513` vor dem geordneten Stoppen. Der versteckte
  `FullSweepPpCkpt`-Test kann mit leerer Fixture-Warnung bestehen — kein Beweis
  für einen gefüllten Sweep.
- **Kompatibilität/Performance/Daten.** PIBD schreibt dasselbe On-Disk-Layout wie
  ZIP; Staging-/Aktivierungsverzeichnisse sind neu. Validierung dominiert den
  Erst-Sync (~1987 s im Messlauf, größtenteils Range Proofs).
- **Rest.** Mainnet, Neustart auf jeder PIBD-Stufe, feindliche Peers und eine
  gefüllte Full-Sweep-Fixture sind **NOT VERIFIED**. Legacy-ZIP-Fallback beim
  Kandidaten nicht erneut geprüft.
- **Links.** [../ARCHITECTURE.de.md](../ARCHITECTURE.de.md),
  [../evidence/INDEX.de.md](../evidence/INDEX.de.md).

## 4. Multi-Peer-PIBD-Scheduler

- **Ausgangsverhalten.** Frühes PIBD sendete Segmentanfragen einfach
  round-robin/an den ersten Peer; Antworten wurden ohne Bindung an den
  angefragten Peer akzeptiert.
- **Problem/Bedingungen.** Ein langsamer oder böswilliger Peer konnte Segmente
  halten; Sendefehler ließen Anfragen hängen; späte/doppelte Antworten störten
  die Buchführung. Festgestellt durch Design-Review und deterministische Tests,
  nicht durch einen Live-Angriff.
- **Untersuchung.** Ein Peer-Binding-/Per-Peer-Limit-Vertrag wurde geschrieben
  und mit dem Rust-Anfrage-/Antwortmodell verglichen.
- **Änderung.** `PibdScheduler` weist Anfragen dem am wenigsten belasteten Peer
  innerhalb globaler/Per-Peer-Limits zu; gibt bei Sendefehler und Disconnect
  frei; lehnt Fremd-Peer-Antworten ohne Segmentverlust ab; akzeptiert validierte
  späte Antworten; behandelt Duplikate als veraltet; ungültige Payloads schlagen
  fail-closed ohne falschen Ban fehl.
- **Komponenten.** `src/P2P/PIBD/PibdScheduler.h`, `PibdSync.h`,
  `src/P2P/Sync/StateSyncer.*`, `src/P2P/MessageProcessor.cpp`.
- **Tests/Ergebnisse.** `PibdScheduler*`: 68 Assertions / 9 Fälle PASS. Normaler
  Live-Multi-Peer-Download beobachtet (9 Peers, `4998/4998`). Live-feindliches
  Verhalten und False-Ban auf P2P-Ebene sind **NOT VERIFIED**.
- **Kompatibilität/Performance/Daten.** Kein Formatwechsel; bessere Resilienz.
  Live-Durchsatz N Peers gegen 1 ist nicht verglichen.
- **Rest.** Live-Matrix N vs 1, Per-Peer-Durchsatz, Ban-Verhalten bei bösartigen
  Segmenten.
- **Links.** [../ARCHITECTURE.de.md](../ARCHITECTURE.de.md).

## 5. KernelIndex

- **Ausgangsverhalten.** `get_kernel` scannte den Kernel-MMR; der Index fehlte
  oder wurde beim Sync nicht aktualisiert, sodass ein großer Zustand bei jeder
  Tip-Änderung einen vollen Rebuild auslösen konnte.
- **Problem/Bedingungen.** Orphan-Verarbeitung konnte einen Block committen,
  während der Index-Append verweigert wurde (veralteter Index); ein Neustart nach
  State-Installation konnte den ganzen Index neu bauen. Der Orphan-Defekt wurde
  mit einem Regressionstest reproduziert.
- **Untersuchung.** Index an Tip/Generation/Root/Count gebunden; RocksDB-
  Transaktionsgrenze und Reorg-/Recovery-Semantik darauf ausgelegt.
- **Änderung.** Persistenter generationsbasierter Index; Apply/Rewind im selben
  Chain-Write wie der Block; Tip-Bindungsprüfungen; begrenzte inkrementelle
  Erweiterung (4096 Höhen / 32768 Kernels) mit Voll-Rebuild-Fallback; Orphan-
  Repair; Fault-Injection-Proxy; Kill-Recovery-Harness.
- **Komponenten.** `src/PMMR/KernelIndex.*`, `Database/KernelIndexState.*`,
  `src/Database/BlockDBImpl.*`, `src/BlockChain/BlockChainImpl.*`,
  `Processors/BlockProcessor.*`, `src/API/Node/Handlers/GetKernelHandler.h`.
- **Tests/Ergebnisse.** Orphan-Regression 17 PASS (vor Fix fehlgeschlagen);
  Write-Faults 81 PASS; Install + Restart 25 PASS; `get_kernel` Direct-Scan und
  Index-Pfad getestet. Kombinierter Live-Lauf: Generation 2, `3 356 610` Kernels,
  Index am Tip aktuell; Neustart behielt Generation 2 ohne Rebuild. Kill-Recovery
  beim Kandidaten: 241 Assertions, Exit 0.
- **Kompatibilität/Performance/Daten.** Zusätzlicher On-Disk-State in der Block-
  DB; eine Nichtübereinstimmung führt zu sicherem Rebuild. Gemessen: ~122 s
  `Ensure` gesamt, 5 715 MB Spitze. Start mit aktuellem Index ~65 s bis REST-
  Bereitschaft auf dem Legacy-Profil.
- **Rest.** Echter RocksDB-Fehler, Post-Commit, Stromausfall und eine frische
  Messserie auf neuem Profil sind **NOT VERIFIED**. Chunked Rebuild ist
  **PLANNED**. `get_kernel`-Schema-Parität nur teilweise geprüft.
- **Links.** [../ARCHITECTURE.de.md](../ARCHITECTURE.de.md),
  [../evidence/INDEX.de.md](../evidence/INDEX.de.md).

## 6. Range-Proof-Validierungs-Worker

- **Ausgangsverhalten.** Range Proofs wurden sequenziell geprüft; historische
  Experimente zeigten mögliche Zeitgewinne durch Parallelität.
- **Problem/Bedingungen.** Die Vollzustandsprüfung dominiert (über 30 Minuten im
  Messlauf). Parallelität darf die Prüfung nicht abschwächen und den Speicher
  einer belasteten Workstation nicht überziehen.
- **Untersuchung.** G-49 maß 2- und 4-Worker-Varianten; die Worker-Politik wurde
  um CPU-Affinität, physische Kerne, verfügbaren Speicher und Windows-Job-Object-
  Limits überarbeitet.
- **Änderung.** Batch-Verifier mit konfigurierbaren Workern (Default 1),
  explizitem `1|2|…` und Opt-in `auto`; Job-Object-Buchhaltung korrigiert
  (Per-Process- und Job-weite committed memory, echte Null, konservativer
  Fallback); Benchmark meldet tatsächlich verifizierte Proofs, nicht PMMR-Größe.
- **Komponenten.** `src/PMMR/RangeProofBatchVerifier.*`,
  `include/Core/Global.h`, `src/Server/Main.cpp`, CPU-/Memory-Probes.
- **Tests/Ergebnisse.** Worker-Policy- und Job-Accounting-Tests bestehen; ein
  synthetischer 10 001-Proof-Benchmark existiert (`1 Worker` am schnellsten im
  kleinen Zustand). Historisch bestand eine Vollzustandsprüfung mit 2 Workern;
  der 4-Worker-Lauf war unter anderen Bedingungen. Vollzustand 1/2/3/4/auto ist
  **NOT VERIFIED**.
- **Kompatibilität/Performance/Daten.** Default bleibt 1, daher kein
  automatisches Performance-Versprechen. `auto` ist explizit.
- **Rest.** Vergleichbarer Vollzustands-Benchmark, automatische Default-Entscheidung,
  Speicherschwellen, Nicht-Windows-Limits.
- **Links.** [../evidence/INDEX.de.md](../evidence/INDEX.de.md).

## 7. Speicherkonsistenz und Recovery

- **Ausgangsverhalten.** Chain-Dateien und RocksDB konnten nach einem Absturz
  inkonsistent bleiben; DB-Repair-Zähler wurden nicht exponiert; Bodyless-Reorg
  konnte den Zustand ändern, bevor fehlende Bodies erkannt wurden; Rust-Kernel-
  PMMR-Archive variabler Größe waren nicht lesbar.
- **Problem/Bedingungen.** Durch Recovery-Tests und synthetische Kill-/Reorg-
  Szenarien beobachtet; echtes Stromausfallverhalten nie festgestellt.
- **Untersuchung.** Atomarer Checkpoint, DB-Recovery/Repair, Reorg-Preflight und
  Rust-v2-Kernel-PMMR wurden gegen die Referenz geprüft.
- **Änderung.** Atomarer `chain.checkpoint`, Repair-Zähler (`db_repaired`,
  `db_last_repair`), Recovery-Kopien, Fail-Closed-Reorg-Preflight mit typisiertem
  `LOCAL_REWIND_DATA_MISSING`, Laden variabler Kernel-PMMR, File-Sync-Reihenfolge
  und geprüfte Chain-/MMR-Invarianten.
- **Komponenten.** `src/BlockChain/ChainState.cpp`, `ChainConsistency.h`,
  `src/BlockChain/BlockChainImpl.cpp`, `src/Database/BlockDBImpl.*`,
  `src/Core/File/*`, `src/PMMR/KernelMMR.*`.
- **Tests/Ergebnisse.** Reorg/Rollback 100 Assertions / 5 Fälle; Bodyless-Safe-
  Refusal besteht; frischer lokaler Legacy-Zyklus und Neustart bestanden
  historisch; kombinierter Live-Lauf startete ohne State-Redownload neu. Kill-
  Harnesses bestehen, sind aber kein Stromausfall.
- **Kompatibilität/Performance/Daten.** Dateien sind mit dem Upstream-Layout
  kompatibel; veralteter/fehlender Index löst Rebuild aus. Der Checkpoint ist
  zusätzlicher Zustand und atomar ausgelegt.
- **Rest.** Stromausfall, volle Platte, echte RocksDB-I/O-Fehler, öffentlicher
  Reorg und ein echter Bodyless-**Reorg** sind **NOT VERIFIED**; unterstützt wird
  nur sichere Verweigerung.
- **Links.** [../ARCHITECTURE.de.md](../ARCHITECTURE.de.md).

## 8. API, Authentifizierung und Diagnose

- **Ausgangsverhalten.** Owner-Endpunkte und `/v1/shutdown` hatten kein Secret;
  Foreign exponierte `update_config`; Owner-`get_config` fehlte; `/v1/status`
  nutzte nur das Legacy-Schema; vier Rust-Owner-Chain-Control-Methoden fehlten.
- **Problem/Bedingungen.** Festgestellt durch statisches Audit von Routen,
  Handlern und Rust-Referenz; ein Foreign-/Read-Only-Client konnte eine Admin-
  Schreiboperation versuchen, jeder lokale Client den Node herunterfahren.
- **Untersuchung.** Zugriffsstufen und Secret wurden aus dem API-Kit-Design
  übernommen, aber neu implementiert; das Status-Schema wurde Methode für Methode
  abgebildet.
- **Änderung.** `ApiAccess` mit 64-Hex-Owner-Only-Secret, Bearer / `X-API-Secret`,
  PUBLIC/PRIVATE/ADMIN-Durchsetzung im `RPCServer`, Loopback-Host-/Origin-Prüfung,
  geschütztes Shutdown/Update_Config/Resync/Ban, Owner-`ban_peer`/`unban_peer`,
  Rust-kompatibles Status-Objekt plus Legacy-Felder, korrigierte REST-Peer-/
  Lastkernels-Formen und transaktionales Config-Update.
- **Komponenten.** `src/Net/Servers/RPC/*`, `src/API/Node/*`,
  `src/Server/Node/NodeRestServer.cpp`, `src/Server/Node/API/*`.
- **Tests/Ergebnisse.** 27 lokale HTTP/RPC-Prüfungen auf Zwischenkandidaten
  (Auth, leere Arrays, ungültige Zahlen, Resync, IPv6-Ban/Unban, Shutdown)
  bestanden; die Suite deckt Handler ab; Owner-Auth ohne Secret liefert den
  erwarteten Fehler. Der finale Kandidat wurde nicht erneut durch die volle
  HTTP-Matrix geführt.
- **Kompatibilität/Performance/Daten.** `GET /v1/peers/<ip>` ist bei neueren
  Kandidaten ein Objekt, `lastkernels` liefert `{hash}`-Objekte; externe
  Verbraucher brauchen ggf. Anpassung. Auth ist eine bewusste Breaking-Änderung
  für Clients, die unauthentifizierten Owner-/Shutdown-Zugriff nutzten.
- **Rest.** Volle Rust-Parität ist **NOT VERIFIED**; vier Owner-Chain-Control-
  Methoden sind **NOT IMPLEMENTED**; PIBD-/Validierungsstatusfelder und Peer-
  Identität (IP vs. SocketAddr) weichen ab; Live-API beim Kandidaten und Mainnet-
  API sind nicht verifiziert.
- **Links.** [../API.de.md](../API.de.md), [../STATUS.de.md](../STATUS.de.md).

## 9. Früher Health und Startbeobachtbarkeit

- **Ausgangsverhalten.** Der HTTP-Server entstand erst nach Öffnen von Chain und
  Datenbank; eine lange Initialisierung wirkte wie ein toter Prozess.
- **Problem/Bedingungen.** Mehrere Minuten Start auf großen Profilen; Operatoren
  und Tools hatten kein Bereitschaftssignal.
- **Änderung.** Früher `/v1/health`-Listener (`starting`/`ready`) vor der Chain,
  danach der volle REST/RPC-Server. `/v1/status` erscheint konstruktiv später.
  Startzeiten werden geloggt.
- **Tests/Ergebnisse.** Erste Health-200 nach `0,56–0,62 s`; `ready` lag 19–25 s
  vor `fully_synced`. Einmal überschritt `/v1/status` nach der Zustandsprüfung
  5 s; Ursache nicht festgestellt.
- **Rest.** `starting` unter konsistent schwerer Last nur teilweise verifiziert;
  Status-Latenz unter Last offen.
- **Links.** [../API.de.md](../API.de.md).

## 10. FastInit Header-Checkpoint (experimentell)

- **Ausgangsverhalten.** Jeder Start führte eine volle Header-Chain-/MMR-Prüfung
  aus, etwa 58–88 s auf dem Profil.
- **Problem/Bedingungen.** Wiederholte Neustarts verbringen die meiste Zeit mit
  der erneuten Prüfung einer unveränderten Header-Chain.
- **Untersuchung.** Ein Checkpoint ist nur sicher, wenn sein Zustand an die
  exakten Chain-Dateien und einen Trust Anchor gebunden ist. Reorg und Crash-
  Consistency sind die Hauptrisiken.
- **Änderung.** Vollständiges `VerifyFull` (Blätter und innere MMR-Knoten,
  Sequenz, Felder, Difficulty, Präfixe, Größen), versionierter Checkpoint,
  Trust-Anchor-gesteuerte Ersterstellung, metadata-gebundene Wiederverwendung,
  Refresh nach Sync, Fallback auf Vollprüfung und intakter alter Checkpoint bei
  unterbrochenem Refresh.
- **Komponenten.** `src/BlockChain/HeaderStateCheckpoint.*`,
  `BlockChainImpl.*`, `src/Server/Main.cpp`, `src/Server/Node/Node.cpp`.
- **Tests/Ergebnisse.** Feldkorruption (Version, Timestamp, Output-Root), innere
  MMR-Korruption, Stale-Fallback, Refresh und normaler Neustart wurden am
  Prototyp-Stand geprüft. Fast Path: `7,8–14,2 s`. Der Merge-/Build-Konflikt wurde
  im kombinierten Quellbaum gelöst, aber ein kombiniertes Live-Szenario, echter
  Reorg und Profil-Lock sind nicht verifiziert.
- **Kompatibilität/Performance/Daten.** Opt-in, standardmäßig aus; ergänzt
  `header.ckpt`; keine Konsensänderung.
- **Rest.** **EXPERIMENTAL**; Reorg, Profil-Lock, Post-Kill-Fortsetzung und
  finale Wiederholmessungen sind NOT VERIFIED. Nicht in Produktion verwenden.
- **Links.** [../STATUS.de.md](../STATUS.de.md).

## 11. Wallet-Änderungen im finalen Diff

Der finale Diff berührt Wallet-Dateien (Send/Finalize/Cancel/Refresh, SQLite-
Schema/Sessions, Slate). Sie sind enthalten, weil das Repository ein vollständiger
Grin++-Fork ist; das Wallet ist aber **NICHT BEREIT** und nicht Teil der Node-
Beta-Abnahme. Sie sind nur durch Unit-Tests der automatischen Suite abgedeckt; es
gibt keine Live-Abnahme für Senden/Empfangen/Recovery. Nicht als Wallet-Release
darstellen.

## Was sich nicht geändert hat

- Konsensregeln, Genesis, PoW-Prüfung und Blockbelohnung wurden nicht absichtlich
  geändert; der Node folgt den Grin-5.5.2-Regeln (NRD auf Nicht-Mainnet aktiv,
  wie Upstream).
- Upstream-Lizenz (MIT) und Copyright bleiben erhalten; siehe `LICENSE`,
  `THIRD_PARTY_NOTICES.md`.
- Die UI ist nicht Teil dieses Repositories.

## Evidenzgrenzen

- Jedes PASS ist an ein konkretes Binary und Szenario gebunden; siehe
  [../evidence/INDEX.de.md](../evidence/INDEX.de.md).
- Historische Ergebnisse früherer Build-IDs sind als historisch markiert und
  werden nie auf das Kandidaten-Binary übertragen.
- Wo ein Defekt nur durch Analyse festgestellt wurde, ist das ausdrücklich
  gesagt: nicht jeder Defekt wurde live reproduziert.

---

[English version](FROM-UPSTREAM-TO-CURRENT.en.md) ·
[Русская версия](FROM-UPSTREAM-TO-CURRENT.ru.md)
