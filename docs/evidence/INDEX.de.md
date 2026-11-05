# Evidenzindex (deutsche Kurzfassung)

Der kanonische und vollständige Index ist [INDEX.md](INDEX.md). Diese Datei ist
eine Kurzfassung für deutschsprachige Leser; bei Abweichungen gilt die englische
Datei. Originale, SHA-256 und Redaktionsregeln stehen in
[ORIGINALS.md](ORIGINALS.md).

## Umgebung

Windows 11 x64, Visual Studio 2022 MSVC 14.44, Release `/W4 /WX`, statische CRT.
Kandidat: `node-beta.1-candidate`, `GrinNode.exe` `4039086C…`, `Tests.exe`
`A9594508…`, Quellmanifest `964B2DD8…` (1528 Dateien).

## Öffentliche Dateien (`docs/evidence/files/`)

| Datei | Inhalt |
|---|---|
| `tests-full-final.log` | Vollständige Suite: 373 Fälle / 4982 Assertions, Exit 0 |
| `target-final.json` | Gezielt: Orphan 17, Faults 81, PIBD-Install 25, PibdScheduler 68/9, BlockPipe 41/6 |
| `tests-fullsweep.log` | FullSweep: 6 Assertions PASS mit Leer-Fixture-Warnung |
| `nrd-kill-manifest.json` | Manifest des erzwungenen Kill/Recovery |
| `nrd-kill-child.stdout.log`, `nrd-kill-recovery.stdout.log` | Ausgabe des getöteten Prozesses und der Recovery (241 Assertions, Exit 0) |
| `combined-live-floonet-2026-11-05.md` | Bereinigter Auszug des Live-Floonet-Laufs auf dem kombinierten Build `16C24DB4…` |
| `candidate-live-floonet-2026-11-05.md` | PASS (begrenzt) des Live-Laufs auf dem Kandidaten `4039086C…` |
| `pibd-segment-counts-2026-11-05.md` | Primäre `pibd_download_completed`-Zeilen: Kandidat `4999/4999`, kombinierter Build `4998/4998` |
| `candidate-live-node-log-excerpt-2026-11-05.log` | Bereinigter Log-Auszug des Kandidaten (Pfade/IPs entfernt) |
| `candidate-source-manifest-final.sha256` | Finales Quellmanifest |

## Zentrale Aussagen

| ID | Aussage | Status |
|---|---|---|
| E-01 | Vollständige Suite auf dem Kandidaten | **PASS** |
| E-02 | Gezielte Index-/Scheduler-/BlockPipe-Tests | **PASS** |
| E-04 | NRD-Kill/Recovery | **PASS** (erzwungener Kill, kein Stromausfall) |
| E-06 | Lokaler Release-Build des Kandidaten | **PASS (lokal)** |
| E-07 | Floonet-Live-PASS auf `16C24DB4…` | **PASS (binary-gebunden)**; `4998/4998` bei Horizont 3.320.640 |
| E-08 | Floonet-Live-Lauf auf dem Kandidaten `4039086C…` | **PASS (begrenzt)**: PIBD ab 3.321.360, `4999/4999` Segmente, 2.948.812/2.948.812 Proofs (1 Worker), `FULLY_SYNCED` 3.324.513, Neustart mit aktuellem Index, `bans=0` |
| E-08b | Abgleich der Segmentzähler | **PASS**: Die Ein-Segment-Differenz folgt aus der anderen Horizonthöhe; Zähler werden nicht zwischen Builds übertragen |
| E-09 | Mainnet | **NOT VERIFIED** |
| E-11/E-12 | KernelIndex Orphan/Faults/Install | **PASS** (lokale Tests) |
| E-14 | Echter RocksDB-Fehler / Stromausfall | **NOT VERIFIED** |
| E-18 | Multi-Peer-PIBD-Politik | **PASS** (deterministische Tests) |
| E-19 | Lokale API-Matrix (27 Prüfungen) | **PASS** auf Zwischenbinaries; beim finalen nicht wiederholt |
| E-20 | 4 Owner-Chain-Control-Methoden | **NOT IMPLEMENTED** |
| E-22 | FastInit | **EXPERIMENTAL / NOT VERIFIED** |
| E-23 | PIHD | **PLANNED / EXPERIMENTAL** |
| E-24 | Wallet/UI | **NOT READY** |

## Regeln

- Ein übersprungener Test, eine fehlende Fixture, ein Timeout oder ein Mock ist
  kein PASS.
- Ein Ergebnis gehört zu Binary und Szenario; keine Übertragung auf einen anderen
  SHA.
- Das ältere `source-manifest.sha256` ist nur Provenienzgeschichte; gültig ist
  das finale Manifest.
