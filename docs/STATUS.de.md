# Status — Node-Beta-1-Kandidat

Diese Seite ist die maßgebliche Statusaussage für den Fork-Stand. Kanonisch ist
Englisch; Übersetzungen: [STATUS.md](STATUS.md) (EN) und
[STATUS.ru.md](STATUS.ru.md) (RU).

Statusvokabular der gesamten Dokumentation:

| Label | Bedeutung |
|---|---|
| **PASS** | Beobachtet auf dem genannten Commit und Binary, in der genannten Umgebung und Szene; Evidenz ist erfasst. |
| **PARTIAL** | Implementiert und teilweise geprüft; mindestens eine geforderte Szene, Umgebung oder ein Verbraucher ist nicht verifiziert. |
| **FAIL** | Beobachteter Fehler in einer definierten Szene. Wird nicht verwendet, wenn ein Test nur übersprungen wurde. |
| **NOT VERIFIED** | Keine qualifizierende Evidenz. Ein übersprungener Test, eine fehlende Fixture, ein Timeout oder ein Mock ist kein PASS. |
| **PLANNED** | Nur Entwurf/Plan; keine abgenommene Implementierungsevidenz. |
| **EXPERIMENTAL** | Code vorhanden, nur Opt-in; nicht für Produktion oder Normalbetrieb empfohlen. |

## Release-Identität

| Punkt | Wert |
|---|---|
| Stufe | **Node-Beta-1-Kandidat**; das Label Beta 1 wird mit diesem Dokument nicht vergeben |
| Build-/Quell-Tag | `node-beta.1-candidate` — annotiertes Objekt `fc5174ec…`, getaggter Commit `ccc4f356…` |
| Release-Tag | `node-beta.1` — zeigt auf den finalen Dokumentations-Commit; das Tag-Objekt wird mit dem Release erfasst |
| Upstream-Basis | GrinPlusPlus `v1.2.9`, Commit `a0958a391c30cf630e3e08534fbc6fe9fb194326` (22.10.2024) |
| `GrinNode.exe` SHA-256 | `4039086CE339106DE37D7C6A98D54873171458699507DB18E938AFD938C80EFC` |
| `Tests.exe` SHA-256 | `A9594508FBDF5B48AA123A862CBB68B20882AD98BB8AA437A9E9A9C60D4FAC8C` — nur zur Verifikation, nicht im Nutzerpaket |
| Quellmanifest des Kandidaten | `1528` Dateien, SHA-256 `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627` |
| Verifizierte Plattform | Windows 11 x64, MSVC 14.44 (Visual Studio 2022), Release, `/W4 /WX`, statische CRT |

Ein Ergebnis gehört zu dem Binary und Quellbaum, die es erzeugt haben. Alle
Ergebnisse unten gelten für den Kandidaten, sofern nicht anders angegeben. Der
frühere kombinierte Build `GrinNode.exe` SHA-256
`16C24DB404702595EDB4CEDA7A057D2C187339FB92664473D211DFDB5F3FF3A2`
wird nur zitiert, wo er das getestete Artefakt ist.

PIBD-Segmentzähler sind laufspezifisch: Der Kandidat startete bei Horizont
`3 321 360` und meldet `4999/4999`; der frühere kombinierte Build startete bei
`3 320 640` und meldet `4998/4998`. Die Differenz von einem Segment folgt aus der
anderen Horizonthöhe und ist kein Defekt; Zähler werden nie zwischen Builds
übertragen.

## Node-Beta-1-Abnahmekriterien

Die Kriterien stammen aus dem Node-Beta-Dossier. Sie stehen hier, damit
„Beta-1-Kandidat“ nicht mit „angenommener Beta 1“ verwechselt wird.

| # | Kriterium | Status | Grundlage / Restlücke |
|---|---|---|---|
| 1 | Vollständiger Mainnet-Sync von Null bis zur aktuellen Höhe und weitere Blockannahme auf einem finalen SHA, ohne künstlich kurze Peer-Liste | **NOT VERIFIED** | Mainnet wurde mit dem Kandidaten nicht gestartet. Floonet ist nicht Mainnet. |
| 2 | Vollständiges PIBD auf einem finalen SHA: Segmente → Prüfung → Aktivierung → Aufholen, mit Negativfällen und Zustandserhalt | **PASS (begrenzt)** | Kandidat `4039086C…`, frisches isoliertes Floonet-Profil, 10 Peers: PIBD ab `3 321 360` (`17d0428f…`), `4999/4999` Segmente; `2 948 812/2 948 812` Range Proofs in `1 215 538 ms` mit 1 Worker; `Successfully validated TxHashSet` um 00:44:15; KernelIndex-Generation 2 mit `3 357 330` Kernels; `PIBD state sync completed and validated` um 00:46:40; Aufholen; geordneter Neustart 00:53:13→00:53:21 mit `kernel_index_check result=current` (Generation 2, Index bei `3 324 510`, `3 360 496` Kernels) und **kein Rebuild**; `FULLY_SYNCED` bei Höhe `3 324 513` (Netz `3 324 513`); geordnetes Stoppen 00:55:53; `bans_fraud_height=0`, `bans_invalid_data=0`. Negativfälle und Zustandserhalt sind durch Tests abgedeckt, nicht durch diesen einen Live-Lauf. |
| 3 | Neustarts über Sync-Stufen: Header, PIBD-Download, Prüfung/Aktivierung, Aufholen; regulärer und kontrollierter Neustart | **PARTIAL** | Neustart nach abgeschlossenem Sync ist **PASS (begrenzt)** beim Kandidaten (Neustart 00:53, `kernel_index_check result=current`, kein Rebuild) und beim kombinierten Build. Neustart während PIBD-Download/Prüfung und eine Stufenmatrix sind NOT VERIFIED. |
| 4 | Validierungsoptimierung aktiv und unter vergleichbaren Bedingungen nachgewiesen | **PARTIAL** | Parallele Range-Proof-Prüfung existiert; Release-Default ist 1 Worker. Der Vollzustands-Benchmark 1/2/3/4/auto und die automatische Default-Wahl bleiben OPEN. |
| 5 | Vollständige API-Konsistenz ohne offene Legacy-Probleme, live auf einem SHA geprüft | **NOT VERIFIED** | Registrierte Methoden funktionieren lokal; Schema-Parität pro Methode, mehrere PIBD/Validierungs-Statusfelder und vier Owner-Chain-Control-Methoden sind offen. Siehe [API.de.md](API.de.md). |
| 6 | Release-Evidenz: exakte Basis, eingefrorene Quellen, Build-Parameter, Hashes, ausreichende Regression | **PARTIAL** | Finales Manifest, Binaries, vollständiges Testlog, gezielte Logs, Live-Floonet-PASS (begrenzt) und PASS des sauberen Neu-Builds existieren. Hosted CI wurde nicht ausgeführt; Mainnet gehört nicht zu diesem Kriterium, bleibt aber ungetestet. |

## Implementiert und verifiziert

| Bereich | Status | Evidenz / Umfang |
|---|---|---|
| Vollständige automatische Suite auf dem Kandidaten | **PASS** | `Tests.exe "~[.]"` (`Tests.exe` SHA `A9594508…`): 373 Testfälle, 4982 Assertions, Exit 0, Lauf 05.11.2026 23:50–23:51. Fixture-abhängige Prüfungen werden übersprungen, nicht bestanden. |
| Wiederherstellung des Kernel-Index nach Prozess-Kill | **PASS** | Kandidat `A9594508…`: erzwungener Kill an einem Transaktionspunkt + `RebuildKillRecovery`, **241 Assertions, Recovery Exit 0**. Der frühere kombinierte Build `16C24DB4…` hatte ein Entry/State-Harness-Paar mit je 219 Assertions. Recovery prüft alten Tip/Count/Checksum und fehlende Teilrecords. Das ist ein erzwungener Kill, kein Stromausfall. |
| Orphan-Regression des KernelIndex | **PASS** | Fehler vor dem Fix reproduziert; nach dem Fix 17/81 Assertions. |
| KernelIndex nach TxHashSet-Installation (synthetisches Archiv) | **PASS** | 25 Assertions: Installation, Index am installierten Tip, Neustart ohne Rebuild. Kein Netzwerk-PIBD. |
| Multi-Peer-PIBD-Scheduler-Politik | **PASS** | 68 Assertions / 9 Fälle: Peer-Bindung, globale/Pro-Peer-Limits, Freigabe bei Sende-/Verbindungsfehler, Ablehnung fremder Antworten ohne Segmentverlust, späte/doppelte Antworten. Live-feindliche Peers sind NOT VERIFIED. |
| BlockPipe-Lokalfehlerklassifikation (kein falscher Ban) | **PASS** | 41 Assertions / 6 Fälle. Eine lokale Exception, auch ein injizierter Index-Commit-Fehler, bannt den Peer nicht. Echter RocksDB-Fehler ist NOT VERIFIED. |
| Live-Floonet-Sync auf dem früheren kombinierten Build | **PASS (begrenzt)** | `16C24DB4…`: 10 Verbindungen (PIBD startete mit 9 fähigen Peers), Header `0 → 3 324 027`, PIBD `4998/4998` bei Horizont `3 320 640`, Zustandsprüfung `1987 s`, Index-Generation 2 mit `3 356 610` Kernels, `fully_synced` bei `3 324 064`, Neustart mit aktuellem Index, Endhöhe `3 324 082`. An dieses Binary gebunden; der Zähler ist wegen des anderen Horizonts nicht mit dem Kandidaten vergleichbar. |
| API-Registrierung und lokales Auth-Verhalten | **PASS (begrenzt)** | 27 lokale HTTP/RPC-Prüfungen auf Zwischenkandidaten (`C996B748…`, `36AD1C92…`, `D748B90E…`): Status-Konsistenz, leere Arrays, ungültige Zahlen, Owner-Auth-Fehler, Resync, Shutdown; Owner `ban_peer`/`unban_peer` ohne Secret schlägt fehl, IPv6-Ban/Unban funktioniert. Auf `4039086C…` nicht wiederholt; keine vollständige Schema-Parität. |
| Früher lokaler HTTP-Health | **PASS** | `/v1/health` erste HTTP-200 nach `0,56–0,62 s` während der Initialisierung; `ready` vor vollständigem Sync. Startphasen-Evidenz ist teilweise: `starting` war unter schwerer Last nicht immer zu beobachten. |
| Kandidaten-Release-Build | **PASS (lokal)** | Release x64 `/W4 /WX`; Build-Log erfasst. Siehe [docs/evidence/INDEX.de.md](evidence/INDEX.de.md) E-06b. |
| Live-Floonet-Sync auf dem Kandidaten `4039086C…` | **PASS (begrenzt)** | Frisches isoliertes Floonet-Profil, 10 Peers, PIBD ab `3 321 360`: `4999/4999` Segmente, `2 948 812/2 948 812` Range Proofs (1 Worker, `1 215 538 ms`), TxHashSet validiert, KernelIndex-Generation 2 (`3 357 330` Kernels), Aufholen, geordneter Neustart mit `kernel_index_check result=current` ohne Rebuild, `FULLY_SYNCED` bei `3 324 513`, geordnetes Stoppen, `bans=0`. Evidenz: [docs/evidence/files/candidate-live-floonet-2026-11-05.md](evidence/files/candidate-live-floonet-2026-11-05.md) und [pibd-segment-counts](evidence/files/pibd-segment-counts-2026-11-05.md). Auf dieses Binary/Szenario begrenzt. |
| Sauberer Neu-Build | **PASS** | Sauberer Klon baute Release x64 und bestand dieselbe vollständige Suite (373 Fälle / 4982 Assertions). Saubere Binaries `GrinNode 73F35D64…`, `Tests 7DE498F5…`; nicht bitgleich zum Kandidaten (PE-Timestamps/Quellpfad) bei identischen Quellen, Toolchain, Imports und Tests. Hosted CI wurde nicht ausgeführt. |

## Implementiert, teilweise verifiziert

| Bereich | Status | Offen |
|---|---|---|
| Multi-Peer-PIBD-Download | **PARTIAL** | Normaler Download mit 9 Peers im kombinierten Live-Lauf beobachtet; falsche Segmente/feindliche Peers live nicht getestet. |
| KernelIndex-Lebenszyklus | **PARTIAL** | Apply/Rewind/Orphan/Neustart und Proxy-Fault-Injection bestehen. Echte RocksDB-Read/Write/Commit-Fehler, Post-Commit und Stromausfall sind NOT VERIFIED. |
| Header-Checkpoint (FastInit) | **EXPERIMENTAL** | Nur Opt-in, erfordert expliziten Trust Anchor (`GRINPP_HEADER_CKPT_BOOTSTRAP=1` plus veröffentlichten Anchor). Reorg, Profil-Lock und Post-Kill-Szenario ungeprüft; Messungen keine finale Serie. Kein Teil des normalen Starts. |
| Parallele Range-Proof-Prüfung (G-49) | **PARTIAL** | Funktioniert; Default ist ein Worker. `GRINPP_RANGEPROOF_WORKERS=auto` ist Opt-in. Vergleichbarer Vollzustands-Benchmark 1/2/3/4/auto ist OPEN. |
| HTTP/RPC-API | **PARTIAL** | Alle 13 Foreign-Methoden und fünf Owner-Methoden registriert; vier Rust-Owner-Chain-Control-Methoden nicht implementiert; PIBD-/Validierungs-Statusfelder weichen von Rust ab. Siehe [API.de.md](API.de.md). |
| Chain-Recovery und Checkpoints | **PARTIAL** | Atomarer `chain.checkpoint`, Repair-Zähler und Fail-Closed-Pfade existieren; Stromausfall und echter Datenträgerfehler sind NOT VERIFIED. |
| Peer-Diagnose und Block-Sync-Zähler | **PARTIAL** | Zähler trennen Deadline-Ablauf und Cooldown-Reassignment; langfristige Wiederherstellung mit einem Peer ist NOT VERIFIED. |

## Nach dem ersten Stand abgeschlossen

Der Live-Lauf des Kandidaten und der saubere Neu-Build endeten nach dem ersten
Dokumentationsstand. Ihre Ergebnisse stehen in der Tabelle „Implementiert und
verifiziert“ oben; kein Ergebnis wird auf ein anderes Binary übertragen.

## Nicht verifiziert

- **Mainnet:** kein Lauf mit dem Kandidaten. Aus Floonet keine Mainnet-Reife ableiten.
- **Stromausfall:** Kill-Harness ist kein Stromausfall.
- **Echter Speicherfehler:** echte RocksDB-Write/Commit-Fehler, Post-Commit, volle Platte.
- **Öffentlicher Reorg:** nur synthetische Unit-/Integration-Szenarien.
- **Live-feindliche PIBD-Peers:** falsche Segmente, falsche Bans auf P2P-Ebene.
- **Automatische Range-Proof-Worker:** der Kandidaten-Live-Lauf nutzte den Default mit 1 Worker; `auto` wurde auf einem vollen Live-Zustand nicht geprüft.
- **FastInit:** im Kandidaten-Live-Lauf nicht verwendet (keine `header_checkpoint_*`-Ereignisse); bleibt experimentell.
- **Wallet und UI:** siehe unten.
- **Linux/macOS-Laufzeit:** quellkompatibel, in diesem Fork nicht verifiziert.
- **Langzeitstabilität:** mehrtägiger Betrieb und Stufen-Neustartmatrix.

## Geplant / experimentell

| Punkt | Status | Anmerkung |
|---|---|---|
| PIHD-Header-Segment-Client/Server (G-27) | **PLANNED / EXPERIMENTAL** | Client-Skelett vorhanden, standardmäßig aus; serverseitige Auslieferung und ehrliche Capability-Werbung nicht implementiert. Live-Interop ist NOT VERIFIED. |
| Chunked KernelIndex-Rebuild | **PLANNED** | Spitzen-Rebuild-Speicher `5 715 MB` für ~3,36 Mio. Kernels gemessen; Chunked-Entwurf auf ~2,0–2,5 GB geschätzt, nicht implementiert. |
| Header-Validierungs-Beschleunigung | **PLANNED** | `57,8–88,0 s` pro Start auf dem Profil gemessen; begrenzter paralleler Chunk-Vorschlag ohne Integritätsverlust existiert. |
| Automatischer Worker-Default | **OPEN** | Default bleibt 1, bis der Vollzustands-Benchmark existiert. |
| Wallet/UI-Ausrichtung | **PLANNED** | Wallet-API und UI sind separate Abnahmestrecken. |

## Wallet und UI

Wallet-Code ist im Quellbaum, weil dies ein vollständiger Grin++-Fork ist, aber
**das Wallet gehört nicht zur Node-Beta-Abnahme**. Wallet-Änderungen sind nur
durch Unit-Tests der automatischen Suite abgedeckt; es gibt keine Live-
Wallet-Validierung, keine Sende-/Empfangsabnahme und keine Recovery-Abnahme. Die
Electron-UI ist nicht Teil dieses Repositories und **NICHT BEREIT**; sie darf
nicht als ausgeliefert oder unterstützt dargestellt werden.

## Plattform und Paketierung

- **Verifiziert:** Windows 11 x64, Visual Studio 2022 MSVC 14.44, Release
  `/W4 /WX`, statische CRT. **Windows 10 x64 wird quellseitig unterstützt, ist
  hier aber NICHT VERIFIZIERT.** Die EXE bindet die statische CRT (`/MT`) und
  importiert auf der getesteten Maschine nur OS-DLLs; Ausführung aus einer
  Netzwerkfreigabe ist NOT VERIFIED.
- Vorgebaute Abhängigkeits-Binaries werden nicht committet. Die Build-Anleitung
  dokumentiert die festgeschriebenen Revisionen und den lokalen Sysroot.
- Der GitHub-Actions-Workflow ist **vorbereitet, aber nicht ausgeführt**. Lokale
  Skripte sind keine Hosted-CI-Evidenz.
- **Lizenzierung:** Der Fork ist MIT. Dreißig aus der Rust-Node `grin` 5.5.2
  abgeleitete Dateien tragen kommentarweise Apache-2.0-Attributionsheader; die
  Datei-Anforderung nach §4(c) ist **erfüllt** (siehe
  [LICENSES/GRIN-DERIVED-FILES.md](../LICENSES/GRIN-DERIVED-FILES.md) und
  [NOTICE](../NOTICE)). `Logo.png` ist **PARTIAL**: Upstream-Asset aus dem
  ursprünglichen Commit `7a4a6619` (17.12.2018, David Burkett), SHA-256
  `52658BE6…`, ohne separate Upstream-Lizenz; bleibt als MIT-Projektasset des
  Upstream erhalten.
- **Paketierung:** Das nutzerseitige Release-Paket enthält `GrinNode.exe` und
  Dokumentation, nicht `Tests.exe`. `Tests.exe` wird nur unter `verification/`
  zur Reproduzierbarkeit bereitgestellt; es muss mit isolierten
  `HOMEDRIVE`/`HOMEPATH`/`TEMP`, freiem Port 3421 (nur eine Instanz) und dem
  Repository-/Build-Wurzelverzeichnis als cwd laufen. Ein Lauf gegen ein echtes
  Nutzerprofil brach mit `0xC0000409` ab. Keine allgemeine sichere Ausführung
  behaupten.

## Evidenz

Jede Aussage ist einem Artefakt in
[`docs/evidence/INDEX.de.md`](evidence/INDEX.de.md) zugeordnet. Ist das Original
nur lokal vorhanden, nennt der Index Pfad und SHA-256; bereinigte öffentliche
Kopien liegen unter `docs/evidence/files/`.

---

[English version](STATUS.md) · [Русская версия](STATUS.ru.md)
