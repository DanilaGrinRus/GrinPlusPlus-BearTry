# Grin++ — Community-Forks (Node-Beta-1-Kandidat)

Dieses Repository ist ein **unabhängiger** Community-Fork von
[GrinPlusPlus](https://github.com/GrinPlusPlus/GrinPlusPlus). Es ist **kein**
offizieller Release des Upstream-Projekts und wird nicht vom Upstream-Team
betreut. Der Fork entwickelt das **Grin++-Node-Modul und seine HTTP/RPC-API**; Wallet und UI gehören
**nicht zum Release- oder Abnahmeumfang** und werden nicht zur Nutzung empfohlen.

| | |
|---|---|
| Upstream-Projekt | https://github.com/GrinPlusPlus/GrinPlusPlus |
| Basis-Tag | `v1.2.9` |
| Basis-Commit | `a0958a391c30cf630e3e08534fbc6fe9fb194326` (22.10.2024) |
| Projektlizenz | MIT (siehe [LICENSE](LICENSE), Upstream-Copyright bleibt erhalten) |
| Entwicklungsstand | **Node-Beta-1-Kandidat** — kein stabiles Release, keine Mainnet-Bereitschaft |
| Build-/Quell-Tag | `node-beta.1-candidate` — annotiertes Objekt `fc5174ec…`, getaggter Commit `ccc4f356…` |
| Release-Tag | `node-beta.1` — zeigt auf den finalen Dokumentations-Commit; das Tag-Objekt wird mit dem Release erfasst |

## Status auf einen Blick

Der vollständige, evidenzbasierte Status steht in
[docs/STATUS.de.md](docs/STATUS.de.md), die offenen Arbeiten in
[docs/ROADMAP.de.md](docs/ROADMAP.de.md).

| Bereich | Status |
|---|---|
| Node (Windows 11 x64, MSVC 14.44) | Der Kandidat `GrinNode.exe` `4039086C…` baut, besteht die vollständige Testsuite und schloss einen Live-Floonet-Sync (`PIBD 4999/4999` Segmente, dann Prüfung, Index-Rebuild und Aufholen) bis `FULLY_SYNCED` mit aktuellem Index nach dem Neustart ab (**PASS, begrenzt**; siehe [docs/STATUS.de.md](docs/STATUS.de.md)). |
| Sauberer Neu-Build | **PASS** — sauberer Klon baut und besteht dieselbe Suite; nicht bitgleich zum Kandidaten (Build-Metadaten) |
| Mainnet | **NICHT GETESTET** |
| Node-HTTP/RPC-API | **TEILWEISE** — 13 Foreign- und 5 Owner-Methoden registriert; `validate_chain`, `compact_chain`, `reset_chain_head`, `invalidate_header` sind **NICHT IMPLEMENTIERT**; volle Rust-Parität wird nicht behauptet |
| Wallet | **NICHT BEREIT / NICHT EMPFOHLEN** — Upstream-Wallet-Code ist nur zur Abstammung und Build-Kompatibilität im Quellbaum; er gehört nicht zum Node-Beta-Release/Abnahme und es gab keine Live-Wallet-Prüfung |
| UI | Nicht in diesem Repository; **NICHT BEREIT** |
| FastInit Header-Checkpoint | **EXPERIMENTELL**, nur Opt-in; im Live-Lauf des Kandidaten nicht geprüft |
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

Das Build-/Quell-Tag ist `node-beta.1-candidate`, das Release-Tag ist
`node-beta.1`. Ein Neubau derselben Quellen erzeugt nicht automatisch eine
bitgleiche EXE; Toolchain, SDK und Pfade können das Ergebnis verändern. Ermitteln
Sie `sha256sum` Ihrer eigenen Artefakte und dokumentieren Sie diese.

`Tests.exe` ist **nicht Teil des nutzerseitigen Release-Pakets**. Es wird separat
unter `verification/` nur zur Reproduzierbarkeit bereitgestellt, weil ein Lauf
gegen ein echtes Nutzerprofil auf der Vorbereitungsmaschine mit `0xC0000409`
abbrach. Falls Sie es ausführen, nutzen Sie isolierte
`HOMEDRIVE`/`HOMEPATH`/`TEMP`; betrachten Sie es nicht als sicher gegenüber einem
Live-Profil.

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
Assertions, Exit 0). `Tests.exe` ist nur für die Verifikation bestimmt und
**nicht** im nutzerseitigen Release-Paket enthalten. Führen Sie es mit isolierten
`HOMEDRIVE`/`HOMEPATH`/`TEMP` aus: Ein Lauf gegen ein echtes Nutzerprofil brach
auf der Vorbereitungsmaschine mit `0xC0000409` ab, daher ist es nicht als sicher
gegenüber einem Live-Profil zu behandeln. Einige Prüfungen benötigen externe
Fixtures oder explizite Tags und werden **übersprungen**, nicht bestanden; die
genaue Liste steht in [docs/BUILD.de.md](docs/BUILD.de.md) und
[docs/STATUS.de.md](docs/STATUS.de.md).

## Sicherheit

Dies ist Entwicklungssoftware für Testnetze. Verwenden Sie sie nicht mit echten
Guthaben. Veröffentlichen Sie keine Seed-Phrasen, privaten Schlüssel,
API-Secrets (`.api_secret`) oder persönlichen Profildaten. Siehe
[SECURITY.md](SECURITY.md).

## Lizenz und Hinweise

Der Fork ist MIT — siehe [LICENSE](LICENSE); das Upstream-Copyright bleibt
erhalten. Dreißig aus der Rust-Node `grin` 5.5.2 abgeleitete Dateien tragen
kommentarweise Apache-2.0-Attributionsheader; die kanonische Liste ist
[LICENSES/GRIN-DERIVED-FILES.md](LICENSES/GRIN-DERIVED-FILES.md), der vollständige
Apache-2.0-Text steht in [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt), der
Komponentenhinweis in [NOTICE](NOTICE). Die Datei-Attribution nach Apache-2.0
§4(c) ist **erfüllt**. Gebündelte Abhängigkeiten und ihre Hinweise sind in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) aufgeführt; vollständige
Lizenztexte stehen in [LICENSES/](LICENSES/README.md).

`Logo.png` ist **PARTIAL**: Es ist das Upstream-Projektasset aus dem
ursprünglichen Commit `7a4a6619` (17.12.2018, David Burkett), SHA-256
`52658BE6…`, ohne separate Upstream-Lizenz. Es bleibt als MIT-Projektasset des
Upstream mit Upstream-Copyright erhalten; für es wird kein fremder Lizenztext
behauptet.

---

[English version](README.md) · [Русская версия](README.ru.md)
