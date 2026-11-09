# Components — Api / Wallet / UI / Tests / Node

## Node

- `Node/Beta` — frozen Beta binary/source.
- `Node/RC1` — reference RC1.
- `Node/Beta/Source` — recovered buildable source, provenance в
  `Source/SOURCE_PROVENANCE.json`.

## Api

- `Api/Source/src/API` — node/wallet/owner/foreign API.
- `Api/Source/include/API` — headers.
- `Api/Source/tests/src/API` — тесты.

## Wallet

- `Wallet/Source/src/Wallet` — wallet core/DB/keychain/slate/refresher.
- `Wallet/Source/include/Wallet` — headers.
- `Wallet/Source/tests/src/Wallet` — тесты.
- `Wallet/GrinDbTool.exe` — `8C0113D2…`.
- `Wallet/Support` — wallet TorConfig/API overlay, применяемый поверх
  `Node/Beta/Source`.

## UI

- `UI/Source` — GrinPlusPlusUI React/Electron source.
- `package.json`, `tsconfig.json`, `webpack.config.js` — сборка UI.
- UI не входит в Beta node acceptance.

## Tests

- `Tests/Tests.exe` — `96E80CBB…` (new pair; frozen old reference kept in `reference/Beta-B565`).
- Frozen old pair full suite: **PASS — 468/468, 19 309 assertions**.\n- Rebuilt new pair full suite: **PASS — 471/471, 19 340/19 343/19 346 assertions**.
- Targeted PASS: W-06, Tor*, P2P*, SyncLogGate, TxHashSet progress, RateLimit,
  HandShake, MessageHeader. `TorConfig*` не входит в frozen B565 Tests.exe
  (нужен rebuild overlay Tests) — NOT VERIFIED.

## Release layout

```text
Node/{Beta,RC1}
Api/Source
Wallet/{Source,Support,GrinDbTool.exe}
UI/Source
Tests
docs/{BETA,SOURCE,OPERATIONS,COMPONENTS}.md
src/grinplusplus
.toolchain
reference
```
