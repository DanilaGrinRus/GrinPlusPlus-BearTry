# Bauen, testen und starten

Dies ist die kanonische Build-Anleitung. Sie gilt für den Quellbaum mit dem Tag
`node-beta.1-candidate`. Die Datei [`BUILD.md`](../BUILD.md) im Wurzelverzeichnis
ist ein Verweis auf dieses Dokument.

## 1. Voraussetzungen

**Verifizierte Toolchain (alle PASS-Ergebnisse in diesem Repository):**

- Windows 11 x64.
- Visual Studio 2022 MSVC 14.44 (v143) mit Windows-SDK 10/11.
- CMake 3.20 oder neuer (das Projekt deklariert 3.8, getestet wurde der
  VS-2022-Generator).
- PowerShell 7 oder Windows PowerShell 5.1.
- Python 3.10+ *nur* für das Sysroot-Helferskript (optional).
- Mindestens 16 GB RAM für den Build; der Node selbst belegt beim normalen Start
  etwa `2,4–2,5 GB` Working Set und bis zu `5,7 GB` bei einem vollständigen
  Kernel-Index-Rebuild auf einem Zustand mit ~3,36 Mio. Kernels.

**Quellseitig unterstützt, in diesem Fork nicht verifiziert:** Windows 10 x64
und Linux x64 mit GCC. `CMakeLists.txt` lehnt Clang ab, daher braucht macOS GCC
und ist hier ebenfalls nicht verifiziert. Stellen Sie Windows-10-, Linux- oder
macOS-Binaries nicht als getestet dar.

## 2. Abhängigkeiten

Das Projekt linkt eine statische CRT (`/MT`) und erwartet vorgebaute
Bibliotheken mit CMake-Package-Configs. Es gibt zwei Wege.

### 2.1 Vorgebautes Sysroot (der Weg für den Kandidaten-Build)

Der Kandidat wurde gegen ein lokales Windows-x64-Sysroot gebaut, das aus den
unten festgeschriebenen Revisionen besteht. Vorgebaute Binaries werden **nicht**
ins Repository committet (etwa 162 MB). Beziehen Sie das Sysroot aus dem
Release-Paket oder bauen Sie eines mit denselben Pins. Das Referenz-Sysroot des
Kandidaten hat 746 Dateien und 162.287.187 Bytes; die Manifest-SHA-256 ist
`82FBF0F7F2D5E5BFFB8C049479E677A10F47177E6A55459269960EAD54EC5C53`.
Nutzen Sie dieses Manifest als Integritätsanker, wenn Sie ein vorgebautes Bündel
von Dritten erhalten.

```powershell
# Der portable Helfer (tools/deps/build_sysroot.py) wird als geprüfter Ausgangspunkt veröffentlicht.
python tools/deps/build_sysroot.py --dry-run   # zuerst prüfen
python tools/deps/build_sysroot.py
# Danach das Präfix übergeben:
#   -DCMAKE_PREFIX_PATH=<absoluter Pfad zum Sysroot>
```

**Status des Abhängigkeits-Rebuilds:** Der portable Helfer wurde geprüft und im
Dry-Run ausgeführt, aber während der Public-Fork-Vorbereitung **nicht
End-to-End** ausgeführt. Das veröffentlichte Sysroot entstand mit der früheren
lokalen Pipeline aus denselben Pins. „Sysroot mit diesem Helfer neu bauen“ gilt
als **NOT VERIFIED**, bis jemand es reproduziert und den Manifest-Hash prüft.

Ist der Helfer nicht vorhanden, reproduzieren Sie die Pins aus der Tabelle. Ein
Sysroot-Manifest mit SHA-256 wird mit der Release-Evidenz veröffentlicht; nutzen
Sie es als Integritätsanker, wenn Sie ein vorgebautes Bündel von Dritten
erhalten.

### 2.2 vcpkg (Upstream-Weg)

Das Upstream-Projekt nutzte vcpkg-Commit
`6185aa76504a5025f36754324abf307cc776f3da` (2020.06) mit den Overlay-Ports und
-Triplets in `vcpkg/` und der Paketliste `vcpkg/packages.txt`. Dieser Weg ist
älter als VS 2022; er ist aus Kompatibilitätsgründen dokumentiert, aber mit der
hier verwendeten VS-2022-Toolchain **nicht verifiziert**.

### 2.3 Festgeschriebene Revisionen

| Abhängigkeit | Revision |
|---|---|
| zlib / minizip | 1.2.11 |
| libsodium | 1.0.18 |
| secp256k1-zkp | `5b39fbf7f1e16e126404791923511723c4c0876d` |
| RocksDB | 6.6.4 (`551a110918493a19d11243f53408b97485de1411`) |
| civetweb | `2c1caa6e690bfe3b435a10c372ab2dcd14b872e8` |
| CRoaring | 0.2.66 |
| fmt | 6.2.1 |
| asio | 1.12.2 |
| mio | `cafa31360fee8866be89b4c602d8b9a7a18dbf5e` |
| Catch2 | 2.12.1 |
| vcpkg (Upstream-Pin) | `6185aa76504a5025f36754324abf307cc776f3da` |

In `deps/` gebündelt und aus diesem Baum gebaut: bitcoin, reproc, stduuid,
cppcodec, uint128, caches, scheduler, spdlog, ghc::filesystem, jsoncpp,
Catch2-Header, scrypt, chachapoly, sqlite3, rfc6234, sha3, klib. Hinweise stehen
in [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

## 3. Quellen beziehen

```powershell
git clone <repository-url> grinpp
cd grinpp
git checkout node-beta.1-candidate
```

Es gibt keine erforderlichen Submodule; `deps/` ist vendored. Kopieren Sie keine
maschinenspezifischen Profile, `bin/`, Logs oder Build-Verzeichnisse in den
Quellbaum.

## 4. Konfigurieren und bauen

```powershell
$sysroot = "C:\path\to\sysroot"   # absoluter Pfad; muss CMake-Configs enthalten
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=$sysroot" `
  -DGRINPP_TESTS=ON `
  -DGRINPP_TOOLS=OFF
cmake --build build --config Release --target GrinNode Tests --parallel 12
```

Erwarteter Ausgabeort (das Projekt schreibt in den Quellbaum):

```text
bin\Release\Release\GrinNode.exe
bin\Release\Release\Tests.exe
```

`-DGRINPP_TOOLS=OFF` baut nur Node und Tests. `slate_tool` und `tx_verifier`
sind Hilfswerkzeuge und für ein Node-Release nicht nötig.

Scheitert MSBuild in einer eingeschränkten Umgebung mit File-Tracker- oder
Named-Pipe-Fehlern, bauen Sie ohne parallele MSBuild-Knoten (`--parallel 1`) und
halten Sie das Temp-Verzeichnis kurz (`$env:TMP = $env:TEMP = "C:\tmp"`).

Build-Ergebnisse sind nicht garantiert bitgleich zwischen Maschinen:
Compiler-Version, SDK, Pfade und Link-Reihenfolge verändern das Binary. Erfassen
Sie die SHA-256 Ihrer eigenen Artefakte.

## 5. Tests

`Tests.exe` ist **nicht im nutzerseitigen Release-Paket** enthalten; es wird
separat unter `verification/` nur zur Reproduzierbarkeit bereitgestellt. Es
bindet den fest kodierten Wallet-Owner-Port **3421**: nur eine Instanz starten,
Port 3421 muss frei sein, und das Arbeitsverzeichnis muss die Repository-/Build-
Wurzel sein. Nutzen Sie isolierte `HOMEDRIVE`/`HOMEPATH`/`TEMP`; ein Lauf gegen
ein echtes Nutzerprofil brach auf der Vorbereitungsmaschine mit `0xC0000409` ab.
Keine allgemeine sichere Ausführung behaupten. Der bekannte gute Volllauf ist der
saubere Build des Release-Strangs (`373` Fälle / `4982` Assertions).

Führen Sie Tests immer in einem isolierten Home-Verzeichnis aus. Die
Test-Binaries schreiben Profile unter `%HOME%\.GrinPP`; ein Lauf gegen ein
echtes Profil kann es verändern.

```powershell
$testRoot = Join-Path $PWD ("test-run-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force (Join-Path $testRoot "temp") | Out-Null
$rootPath = [IO.Path]::GetPathRoot($testRoot).TrimEnd('\')
$env:HOMEDRIVE = $rootPath
$env:HOMEPATH  = $testRoot.Substring($rootPath.Length)
$env:TEMP = $env:TMP = Join-Path $testRoot "temp"

& .\bin\Release\Release\Tests.exe "~[.]"
```

Auf dem Kandidaten erscheint:

```text
All tests passed (4982 assertions in 373 test cases)
```

Versteckte Tags (`[.]`) werden durch `~[.]` ausgeschlossen; führen Sie sie
explizit aus, wenn ihre Fixtures vorhanden sind:

```powershell
.\bin\Release\Release\Tests.exe "[orphan]"
.\bin\Release\Release\Tests.exe "[fault]"
.\bin\Release\Release\Tests.exe "[pibd]"
.\bin\Release\Release\Tests.exe "PibdScheduler*"
.\bin\Release\Release\Tests.exe "BlockPipe*"
.\bin\Release\Release\Tests.exe "REORG*"
.\bin\Release\Release\Tests.exe "PibdSegmentService::FullSweepPpCkpt"
```

Hinweise:

- Externe PIBD-/Live-Vektoren werden nicht ausgeliefert. Fehlt eine Fixture, wird
  die Prüfung **übersprungen** — das ist kein PASS. `FullSweepPpCkpt` kann mit
  leerem Sweep (`bitmap 0/7, output 0/1678, …`) und Warnung bestehen; das prüft
  keinen gefüllten Sweep.
- Das Kill-Recovery-Harness wird über `tools/audit/kernel-index-crash.ps1`
  gestartet und benötigt die `Tests.exe` aus demselben Quellbaum. Es erzwingt
  einen Kill an einem gewählten Transaktionspunkt; es ist kein Stromausfall-Test.
- Starten Sie niemals zwei Test-Binaries gegen dasselbe Profil oder denselben
  Port. Verwenden Sie pro Lauf eindeutige Ports und Home-Verzeichnisse.

## 6. Node starten

```powershell
# Headless-Floonet-Node ohne Wallet, Standardprofil
.\bin\Release\Release\GrinNode.exe --floonet --headless --no-wallet
```

Nützliche Optionen:

| Option | Bedeutung |
|---|---|
| `--floonet` | Floonet statt Mainnet |
| `--automated-testing` | Testkette (nicht für Netzwerktests) |
| `--headless` | Keine interaktive Konsolenanzeige |
| `--no-wallet` | Nur Node |
| `--config-path <file>` | Bestimmte `server_config.json` verwenden |
| `--shared-node host:port` | Entfernten Node verwenden (Client-Modus) |

Standardpfade und -ports:

| Punkt | Mainnet | Floonet |
|---|---|---|
| Profil | `%USERPROFILE%\.GrinPP\MAINNET` | `%USERPROFILE%\.GrinPP\FLOONET` |
| Konfiguration | `…\server_config.json` | `…\server_config.json` |
| REST-API | `3413` | `13413` |
| P2P | `3414` | `13414` |
| Daten / Logs | `<profile>\NODE`, `<profile>\LOGS` | ebenso |

Für einen isolierten Lauf erstellen Sie eine Konfiguration mit explizitem
`DATA_PATH` und separaten Ports und starten mit `--config-path`. Beispiel:

```json
{
  "DATA_PATH": "D:\\grinpp-test\\floonet",
  "P2P": { "MIN_PEERS": 3, "MAX_PEERS": 20, "P2P_PORT": 23414 },
  "SERVER": { "REST_API_PORT": 23413 },
  "LOGGER": { "LOG_LEVEL": "DEBUG" }
}
```

Das API-Secret wird beim ersten Start unter `<DATA_PATH>\.api_secret` erzeugt
(64 Hex-Zeichen, DACL nur für den Besitzer). Kopieren Sie es nie in Logs,
Berichte oder Commits. Diagnose-Logs liegen neben der EXE in `logs\`; ist das
Verzeichnis nicht beschreibbar, fällt der Node auf
`<DATA_PATH>\LOGS\node-diagnostics` zurück.

Häufige Fehler:

| Symptom | Ursache / Maßnahme |
|---|---|
| `Could not find a package configuration file provided by …` | `CMAKE_PREFIX_PATH` zeigt nicht auf das Sysroot mit allen Pins. |
| `could not find any instance of Visual Studio` | VS 2022 Build Tools oder C++-Workload fehlt. |
| Node beendet sich mit Bind-/`address in use`-Fehler | REST-/P2P-Port belegt (anderer Node oder Test). `REST_API_PORT`/`P2P_PORT` ändern. |
| Zwei Instanzen auf einem `DATA_PATH` | RocksDB-Lock. Profil nicht zwischen Instanzen teilen; Profil-Lock-Verhalten ist nicht vollständig verifiziert. |
| Start scheint minutenlang zu hängen | Voller Kernel-Index-Rebuild und Header-Prüfung gehören zum Design; siehe [ARCHITECTURE.de.md](ARCHITECTURE.de.md). Nicht killen, außer man akzeptiert einen Rebuild. |
| `GET /v1/status` direkt nach Start 404 | Der Status-Endpunkt wird später als `/v1/health` registriert; für Prozess-Liveness Health nutzen. |
| Owner-RPC `-32001` / REST 401 | Secret fehlt/falsch oder `.api_secret` ist nicht owner-only. ACL korrigieren, nicht die Prüfung abschwächen. |
| Warnungen über übersprungene Tests | Externe Fixtures fehlen. Übersprungen ist NOT VERIFIED, kein PASS. |

## 7. Status der Build-Verifikation

| Punkt | Status |
|---|---|
| Kandidaten-Release-Build `/W4 /WX` | **PASS** (lokales Build-Log) |
| Saubere Konfiguration aus Klon | **PASS** (unabhängige Release-Prüfung) |
| Sauberer Build aus Klon und Tests | **PASS** (sauberer Klon baute Release x64 und bestand die volle Suite 373/4982; Binaries nicht bitgleich zum Kandidaten) |
| CI-Workflow vorbereitet | **JA** (`.github/workflows/ci.yml`) |
| CI auf GitHub Actions ausgeführt | **NEIN** — Hosted-CI ist keine Evidenz für diesen Stand |
| `Tests.exe`-Paketierung | **Nur Verifikation**, nicht im nutzerseitigen Release-Paket (separates `verification/`; isoliertes Home/TEMP, freier Port 3421, Repo-/Build-Wurzel als cwd) |
| Windows-10-x64-Build/Lauf | **NOT VERIFIED** (quellseitig unterstützt) |
| Linux-/macOS-Build | **NOT VERIFIED** |
| EXE importiert auf der getesteten Maschine nur OS-DLLs | **PASS** (Release-Prüfung); Netzwerkfreigabe NOT VERIFIED |

---

[English version](BUILD.md) · [Русская версия](BUILD.ru.md) ·
[Zurück zu README](../README.de.md)
