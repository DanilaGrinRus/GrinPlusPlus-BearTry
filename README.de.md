# Grin++ — Community-Forks (Node-Beta-1-Kandidat)

Dieses Repository ist ein Community-Forks von
[GrinPlusPlus](https://github.com/GrinPlusPlus/GrinPlusPlus). Es ist **kein**
offizieller Release des Upstream-Projekts und wird nicht vom Upstream-Team
betreut. Der Fork konzentriert sich zuerst auf das **Node-Modul** (GrinNode);
Wallet und UI gehören **nicht** zur Node-Beta-Abnahme.

| | |
|---|---|
| Upstream-Projekt | https://github.com/GrinPlusPlus/GrinPlusPlus |
| Basis-Tag | `v1.2.9` |
| Basis-Commit | `a0958a391c30cf630e3e08534fbc6fe9fb194326` (22.10.2024) |
| Projektlizenz | MIT (siehe [LICENSE](LICENSE), Upstream-Copyright bleibt erhalten) |
| Entwicklungsstand | **Node-Beta-1-Kandidat** — kein stabiles Release, keine Mainnet-Bereitschaft |
| Git-Tag | `node-beta.1-candidate` — annotiertes Objekt `fc5174ec…`, getaggter Commit `ccc4f356…` |

## Status auf einen Blick

Der vollständige, evidenzbasierte Status steht in
[docs/STATUS.de.md](docs/STATUS.de.md), die offenen Arbeiten in
[docs/ROADMAP.de.md](docs/ROADMAP.de.md).

| Bereich | Status |
|---|---|
| Node (Windows 11 x64, MSVC 14.44) | Der Kandidat baut, besteht die vollständige Testsuite und schloss einen Live-Floonet-Sync bis `FULLY_SYNCED` mit aktuellem Index nach dem Neustart ab (**PASS, begrenzt**; siehe [docs/STATUS.de.md](docs/STATUS.de.md)). |
| Sauberer Neu-Build | **PASS** — sauberer Klon baut und besteht dieselbe Suite; nicht bitgleich zum Kandidaten (Build-Metadaten) |
| Mainnet | **NICHT GETESTET** |
| Node-HTTP/RPC-API | **TEILWEISE** — registrierte Methoden funktionieren lokal, vollständige Rust-Parität wird nicht behauptet |
| Wallet | **NICHT BEREIT** — aus Upstream enthalten, mit einigen Korrekturen, für dieses Release nicht abgenommen |
| UI | Nicht in diesem Repository; **NICHT BEREIT** |
| FastInit Header-Checkpoint | **EXPERIMENTELL**, nur Opt-in |
| PIHD (Header-Sync-Erweiterung) | **GEPLANT / EXPERIMENTELL**, standardmäßig aus |
| Multi-Peer-PIBD-Scheduler | Implementiert und deterministisch getestet; Live-Multi-Peer-PIBD im kombinierten Lauf verifiziert |
| Automatische Range-Proof-Worker | Opt-in (`GRINPP_RANGEPROOF_WORKERS=auto`); Standard bleibt 1 |

**Node Beta ist keine Produktreife.** Ein Floonet-PASS ist kein Mainnet-PASS.
Kein Ergebnis wird auf eine andere Binärdatei übertragen: Jede Aussage in der
Dokumentation ist an Commit und SHA der ausführbaren Datei gebunden, siehe
[docs/evidence/INDEX.de.md](docs/evidence/INDEX.de.md).

## Identität des Kandidaten

Finaler Quellbaum und Binärdateien für diesen Dokumentationsstand:

| Artefakt | SHA-256 |
|---|---|
| `GrinNode.exe` | `4039086CE339106DE37D7C6A98D54873171458699507DB18E938AFD938C80EFC` |
| `Tests.exe` | `A9594508FBDF5B48AA123A862CBB68B20882AD98BB8AA437A9E9A9C60D4FAC8C` |
| Quellmanifest des Kandidaten (1528 Dateien) | `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627` |

Commit/Tag des Kandidaten sind in der Repository-Historie veröffentlicht
(`node-beta.1-candidate`). Ein Neubau derselben Quellen erzeugt nicht
automatisch eine bitgleiche EXE; Toolchain, SDK und Pfade können das Ergebnis
verändern. Ermitteln Sie `sha256sum` Ihrer eigenen Artefakte und dokumentieren
Sie diese.

## Verifizierte Plattform

- **Verifiziert:** Windows 11 x64, Visual Studio 2022 MSVC 14.44, Release,
  `/W4 /WX`, statische CRT. Alle PASS-Ergebnisse in diesem Repository stammen
  von dieser Plattform.
- **Quellseitig unterstützt, hier nicht verifiziert:** Windows 10 x64; Linux x64
  mit GCC. Die Upstream-Konfiguration baute auch macOS mit GCC; Clang wird in
  `CMakeLists.txt` abgelehnt. Für Windows 10, Linux und macOS wird kein
  Laufzeitergebnis behauptet.

## Bauen, testen, starten

Voraussetzungen, festgeschriebene Abhängigkeiten und exakte Befehle stehen in
[docs/BUILD.de.md](docs/BUILD.de.md).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=<prebuilt-sysroot>" `
  -DGRINPP_TESTS=ON -DGRINPP_TOOLS=OFF
cmake --build build --config Release --target GrinNode Tests --parallel

# Headless-Floonet-Node ohne Wallet starten
.\bin\Release\Release\GrinNode.exe --floonet --headless --no-wallet
```

Ohne Argumente startet der Node auf **Mainnet** und mit Wallet. Verwenden Sie
für jeden Test ein isoliertes Profil (`--config-path` und `DATA_PATH`) und
richten Sie ihn niemals auf ein Profil mit echten Guthaben.

Standardpfade: Profil `%USERPROFILE%\.GrinPP\<NETWORK>`, Konfiguration
`%USERPROFILE%\.GrinPP\<NETWORK>\server_config.json`. REST/P2P-Ports sind
standardmäßig 3413/3414 auf Mainnet und 13413/13414 auf Floonet.

## Dokumentation

- [docs/STATUS.de.md](docs/STATUS.de.md) — implementiert, verifiziert und offen,
  mit PASS / PARTIAL / FAIL / NOT VERIFIED / PLANNED.
- [docs/ROADMAP.de.md](docs/ROADMAP.de.md) — offene Arbeiten und Exit-Kriterien.
- [docs/ARCHITECTURE.de.md](docs/ARCHITECTURE.de.md) — Node-Aufbau,
  Synchronisierung, Speicher, KernelIndex, FastInit-Vertrauensmodell und
  On-Disk-Layout.
- [docs/API.de.md](docs/API.de.md) — HTTP/REST und JSON-RPC, Authentifizierung,
  Status und bekannte Unterschiede zu Rust Grin 5.5.2.
- [docs/BUILD.de.md](docs/BUILD.de.md) — sauberer Build, Tests, Start, häufige
  Fehler.
- [docs/history/FROM-UPSTREAM-TO-CURRENT.de.md](docs/history/FROM-UPSTREAM-TO-CURRENT.de.md)
  — was sich seit Upstream v1.2.9 geändert hat, warum und mit welcher Evidenz.
- [docs/evidence/INDEX.de.md](docs/evidence/INDEX.de.md) — Zuordnung
  „Behauptung → Evidenz“.
- [KNOWN_ISSUES.md](KNOWN_ISSUES.md) — bekannte Einschränkungen und offene
  Defekte.
- [CHANGELOG.md](CHANGELOG.md) — releaseorientierte Änderungsübersicht.
- [CONTRIBUTING.md](CONTRIBUTING.md) — Mitarbeit und Evidenzanforderungen.
- [SECURITY.md](SECURITY.md) — verantwortungsvolle Offenlegung und sicheres
  Testen.
- [docs/HISTORY-RECONSTRUCTION.md](docs/HISTORY-RECONSTRUCTION.md) — wie die
  Commit-Historie rekonstruiert wurde.

Englisch ist die kanonische Sprache. Weichen Übersetzung und englischer Text
voneinander ab, gilt der englische Text.

## Tests

Die vollständige Suite des Kandidaten besteht (`373` Testfälle, `4982`
Assertions, Exit 0). Einige Prüfungen benötigen externe Fixtures oder explizite
Tags und werden **übersprungen**, nicht bestanden; die genaue Liste steht in
[docs/BUILD.de.md](docs/BUILD.de.md) und [docs/STATUS.de.md](docs/STATUS.de.md).

## Sicherheit

Dies ist Entwicklungssoftware für Testnetze. Verwenden Sie sie nicht mit echten
Guthaben. Veröffentlichen Sie keine Seed-Phrasen, privaten Schlüssel,
API-Secrets (`.api_secret`) oder persönlichen Profildaten. Siehe
[SECURITY.md](SECURITY.md).

## Lizenz und Hinweise

MIT — siehe [LICENSE](LICENSE). Das Upstream-Copyright bleibt erhalten.
Gebündelte Abhängigkeiten und ihre Hinweise sind in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) aufgeführt; vollständige Lizenztexte stehen in [LICENSES/](LICENSES/README.md).

---

[English version](README.md) · [Русская версия](README.ru.md)
