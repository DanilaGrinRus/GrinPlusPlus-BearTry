# Node Beta1 — vorläufige RC1

**Status:** Kandidat für weitere Prüfungen, keine abgenommene Beta1 und keine stabile Veröffentlichung. Vollständiger PIBD von Grund auf, Mainnet und Neustarts in den Synchronisierungsphasen sind mit dieser EXE nicht geprüft.

`GrinNode.exe` — Windows x64, SHA-256 `CD941F325B6E7595E9A777DDA90B486BCAFB0473322CBD95E3FCA5133344FD48`.

Für die **Floonet**-Node ohne Wallet `START_FLOONET_NODE.cmd` öffnen. Das Profil wird in `node-data` neben dem Starter angelegt; der Ordner muss beschreibbar sein. Protokolle stehen in `logs`. Der direkte EXE-Start ohne Parameter wählt standardmäßig Mainnet und aktiviert die Wallet.

- `source/`: Quellcode von Node, API und Tests für den Eigenbau. Einige ältere Hilfswerkzeuge mit lokalen Pfaden wurden ausgelassen; siehe `source/OMITTED_LOCAL_TOOLS.md`.
- `dependencies/sysroot/`: vorkompilierte Windows-x64-Bibliotheken.
- `BUILD.ru.md`: Build- und Testanleitung auf Russisch.
- `evidence/`: Prüfnachweise; ältere Läufe und ihre Pfade sind historisch und keine Laufzeitkonfiguration.
- `docs/`: Audit- und API-Matrix-Snapshots; relative Links in historischen Kopien funktionieren nach einem Umzug möglicherweise nicht.
- `SHA256SUMS.txt`: Dateihashes des Pakets; das vom Benutzer erstellte Archiv und Laufzeitdaten sind nicht enthalten.

MSVC Release /W4 /WX: **362 Testfälle / 4886 Assertions PASS** mit Tests SHA `29F068B92DD575139AED5B2CDC0DC38A274C35B6EAA037A9AE9A4744A65E36E5`. Versteckte Harness-Szenarien und die Live-Fixture waren ausgeschlossen; zwei externe Fixtures fehlten, deren Prüfungen wurden übersprungen. Die PIBD-Segmentanzeige wurde synthetisch getestet; ein Live-Download ist nicht geprüft. Starter, HTTP health und reguläres Beenden bestanden in einem verschobenen Ordner mit Leerzeichen ohne externe Peers.

Die Owner-RPCs `compact_chain`, `validate_chain`, `reset_chain_head` und `invalidate_header` sind in diesem Kandidaten nicht implementiert. Abbruch der Kompaktierung und Neustart nach tatsächlichem Löschen sind nicht geprüft. API und Protokolle in allen Synchronisierungsphasen benötigen Live-Tests.

Das Paket benötigt kompatibles Windows x64 und einen beschreibbaren Ordner; für den Eigenbau zusätzlich CMake und MSVC. Zwei Starts und reguläre Beendigungen wurden auf dem ursprünglichen Computer in einem verschobenen Pfad mit Leerzeichen und kyrillischen Zeichen geprüft, in einer weiteren Umgebung wurde der Start mit Header-Sync vom Benutzer bestätigt; die SHA dieser Kopie wurde nicht gemessen, Netzwerkfreigaben sind ungeprüft. Die übrigen mitgelieferten Drittanbieter-Binärdateien wurden nicht unabhängig aus Quellcode neu erstellt.

Die verwendete statische RocksDB wurde mit Windows-UTF-8-Pfaden und portablen Einstellungen neu erstellt; die enthaltene Shared-Variante wird von der Node nicht verwendet und wurde nicht separat geprüft.

Mit dieser SHA erreichte eine frische Floonet-Node ohne feste Peerliste 5 Verbindungen und begann den Header-Sync (512 von 3323361); vollständiger PIBD und Aufholen sind nicht geprüft. DNS-Seeds entsprechen Grin 5.5.2; ausgehende TCP-Verbindungen unterscheiden fünf Sekunden Timeout und Betriebssystem-Fehlercodes. Ungültige Magic Bytes werden weiterhin abgewiesen; die Diagnose zeigt empfangene und erwartete Bytes. Fehler einzelner Peers bleiben möglich. Der NRD-Differentialtest lief ohne externes Rust-Oracle; dies belegt keine Rust-Parität.

Momentaufnahme der laufenden RC1 am 05.11.2026 um 06:02 UTC+07: 8 Peers, Header 45568 → 49664 in 5 Sekunden; Chain-Höhe 0, Header-Sync läuft. Dies ist keine abgeschlossene Synchronisierung. Siehe evidence/peer-repair/rc1-live-observation.json.
