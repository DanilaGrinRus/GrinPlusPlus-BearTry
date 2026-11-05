# Roadmap

Die Reihenfolge richtet sich danach, was eine ehrliche **Node-Beta-1**-Abnahme
blockiert, danach nach Produktarbeit. Status wie in [STATUS.de.md](STATUS.de.md).
Termine werden nicht versprochen; jedes Exit-Kriterium ist evidenzbasiert.

## Phase 0 — Freeze und Evidenz (aktuell)

Ziel: eindeutige Identität und Evidenz des Kandidaten.

| Punkt | Status | Exit-Kriterium |
|---|---|---|
| Quellen und Binaries einfrieren | **PASS** | Manifest `964B2DD8…` (1528 Dateien); `GrinNode` `4039086C…`; `Tests` `A9594508…` |
| Vollständige Testsuite auf dem Kandidaten | **PASS** | 373 Fälle / 4982 Assertions, Exit 0 |
| Gezielte Index-/Scheduler-/BlockPipe-Tests | **PASS** | `target-final.json` komplett PASS |
| NRD-Kill-Recovery | **PASS** | 241 Assertions, Recovery Exit 0 |
| Veraltetes Manifest bereinigt | **PASS** | In der Doku nur das finale Manifest |
| Rekonstruierte Commit-Historie | **PASS** | `node-beta.1-candidate` (annotiertes Tag-Objekt `fc5174ec…`) zeigt auf Commit `ccc4f356…`; Tree-Verifikation PASS |
| Sauberer Neu-Build | **PASS** | Sauberer Klon baute Release x64 und bestand dieselbe Suite (373/4982); saubere Binaries `73F35D64…`/`7DE498F5…`, nicht bitgleich zum Kandidaten wegen Build-Metadaten |

## Phase 1 — Live-Verifikation des Kandidaten

Ziel: abgeschlossener Floonet-Live-Lauf, an das Kandidaten-Binary gebunden.

| Punkt | Status | Exit-Kriterium |
|---|---|---|
| Floonet-Live-Sync des Kandidaten | **PASS (begrenzt)** | Frisches isoliertes Profil: Header → PIBD → Prüfung → Aufholen → `fully_synced`; Evidenz an `4039086C…` gebunden |
| Neustart nach vollständigem Sync | **PASS (begrenzt)** | Neustart 00:53:13→00:53:21 behielt Generation 2, `kernel_index_check result=current`, kein Rebuild; Fortsetzung bis `FULLY_SYNCED` bei 3.324.513 |
| Neustart während PIBD-Download | **NOT VERIFIED** | Stufe dokumentiert; Zustand erhalten oder sicher neu gestartet |
| Neustart während Prüfung/Aktivierung | **NOT VERIFIED** | Marker-Recovery geprüft; kein Teilzustand live |
| Neustart während Index-Build / erstem Aufholen | **NOT VERIFIED** | Fortsetzung oder sicherer Rebuild; kein Datenverlust |

## Phase 2 — Node-API-Abnahme

Ziel: dokumentierter, getesteter Vertrag für den veröffentlichten Schnitt.

| Punkt | Status | Exit-Kriterium |
|---|---|---|
| Auth-/Routen-Matrix auf einem SHA | **PARTIAL** | 27 lokale Prüfungen auf dem Kandidaten wiederholt; an Binary gebunden |
| Schema-Vergleich pro Methode mit Rust 5.5.2 | **NOT VERIFIED** | Parameter-/Ergebnis-/Fehlertabelle für jede registrierte Methode |
| Owner-Chain-Control-Methoden | **NOT IMPLEMENTED** | Auf echtem Backend mit Security-Review und Tests implementieren oder ausdrücklich nicht unterstützt lassen |
| PIBD-/Validierungs-Statusfelder | **PARTIAL** | Echte Phasenzähler instrumentiert oder Abweichung dauerhaft dokumentiert |
| Peer-Identitätsparität (IP vs. SocketAddr) | **PARTIAL** | Vertragliche Entscheidung dokumentiert und getestet |

## Phase 3 — Resilienz

Ziel: Verhalten bei Fehlern beweisen, nicht nur im Normalbetrieb.

| Punkt | Status | Exit-Kriterium |
|---|---|---|
| Echter RocksDB-Read/Write/Commit-Fehler | **NOT VERIFIED** | Fehler an der echten DB-Grenze injiziert, nicht im Proxy |
| Dateisystemfehler nach Commit | **NOT VERIFIED** | Keine Teilveröffentlichung; Neustart konsistent |
| Stromausfall | **NOT VERIFIED** | Wiederholte plötzliche Abschaltungen auf Testprofil |
| Volle / read-only Platte | **NOT VERIFIED** | Fail-closed mit klarer Fehlermeldung |
| Öffentlicher Reorg (Floonet) | **NOT VERIFIED** | Kontrollierter alternativer Branch auf echtem Netzprofil |
| Feindliche PIBD-Peers | **NOT VERIFIED** | Falsche Segmente, fremder Peer, spät/doppelt, keine falschen Bans live |

## Phase 4 — Performance

Ziel: Defaults durch Messungen entscheiden, nicht durch Vermutungen.

| Punkt | Status | Exit-Kriterium |
|---|---|---|
| Vollzustands-Benchmark 1/2/3/4/auto | **OPEN** | Gleicher Zustand, Wiederholungen, CPU/RSS/Zeit; Default wählen |
| Header-Validierungs-Beschleunigung | **PLANNED** | Integritätserhaltende Chunk-Prüfung; Korruptionen fallen weiterhin durch |
| Chunked KernelIndex-Rebuild | **PLANNED** | Gemessene Spitzenreduktion; Crash zwischen Chunks; Digest-Mismatch |
| Start-Timing auf frischem Profil des finalen SHA | **NOT VERIFIED** | Wiederholbare Serie mit Host-Lastaufzeichnung |

## Phase 5 — FastInit-Entscheidung

| Punkt | Status | Exit-Kriterium |
|---|---|---|
| Reorg auf gültigem alternativem Branch | **NOT VERIFIED** | Checkpoint bleibt gültig oder fällt sicher zurück |
| Profil-Lock | **NOT VERIFIED** | Zweite Instanz scheitert mit klarer Fehlermeldung |
| Fortsetzung nach Kill | **NOT VERIFIED** | Nächster Start erholt sich konsistent |
| Finale Messserie | **NOT VERIFIED** | Kontrollierte Läufe ohne Hintergrundlast |
| Entscheidung | **EXPERIMENTAL** | Fördern, Opt-in behalten oder entfernen — mit Evidenz |

## Phase 6 — PIHD (geplante Erweiterung)

| Punkt | Status | Exit-Kriterium |
|---|---|---|
| Client-Härtung (standardmäßig aus) | **PARTIAL** | Timeout, späte/fremde Antwort, Fallback, kein falscher Ban |
| Server-Header-Segmente + ehrliche Capability | **PLANNED** | Rust-Staging-Live-Loopback in beide Richtungen |
| Laufzeit-Enable/Disable über Owner-API | **PLANNED** | Ohne Neustart; Vertrag dokumentiert |

## Phase 7 — Mainnet-Gate

| Punkt | Status | Exit-Kriterium |
|---|---|---|
| Mainnet-Erstsynchronisierung | **NOT VERIFIED** | Von Null bis aktuelle Höhe auf einem finalen SHA |
| Mainnet-Aufholen und Neustart | **NOT VERIFIED** | Nimmt weiter Blöcke an; Neustart behält Zustand |
| Keine künstlich kurze Peer-Liste | **NOT VERIFIED** | Normale Discovery; sicheres konfigurierbares Verbindungslimit |
| Release-Evidenzpaket | **PARTIAL** | Jede Behauptung an Commit + SHA + Szenario gebunden |

## Getrennte Strecken (nicht Node Beta)

- **Wallet:** Live-Abnahme für Senden/Empfangen/Recovery, Schlüsselbehandlung,
  Migration und Security-Review. Derzeit **NOT READY**.
- **UI:** Ausrichtung am tatsächlichen Backend-Vertrag, Prozess-/Profilsicherheit
  und Ende-zu-Ende-Abnahme in einem separaten Repository. Derzeit **NOT READY**.
- **Flint/Messenger/Own-Network-Forschung:** nur Design; nicht Teil des Releases
  dieses Repositories.

## Stoppregel

Nicht endlos nach immer selteneren Defekten suchen. Wenn die Prüfungen für einen
Modus ausreichen, PASS/FAIL/NOT VERIFIED, Restrisiko und Kandidat festhalten.
Wallet-Daten, Ports, Live-Kopien und Build-Verzeichnisse nie zwischen parallelen
Läufen mischen. Ein Hash oder Mock ersetzt keine Live-Prüfung, und kein Secret,
Seed oder privater Schlüssel darf in Berichte oder Logs gelangen.

---

[English version](ROADMAP.md) · [Русская версия](ROADMAP.ru.md)
