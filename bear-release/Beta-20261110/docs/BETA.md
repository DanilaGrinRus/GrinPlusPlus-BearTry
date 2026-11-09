# Beta — frozen node-only candidate

Дата freeze: 2026-11-10 (rebuilt source).

## Артефакты

| Файл | Размер | SHA-256 |
|---|---:|---|
| `Node/Beta/GrinNode.exe` | 8 476 672 | `B52B6C5D84B25375463634F087E39CD65CF9BF2F3A1E4DB090E7AEAFEBB69C4A` |
| `Node/Beta/Tests.exe` | 11 655 168 | `96E80CBB9BF3C3C5382126A304BF0B628E4B38573E9B84CCAF602ECC471C6708` |
| `Node/Beta/GrinDbTool.exe` | 3 891 712 | `8C0113D2E11A3655DDE2372515974E6B7D832076CC5229C82706616B89F00656` |

Manifest: `docs/FROZEN.json`, `docs/FREEZE_SHA256SUMS.json`.

Source freeze: recovered `Node/Beta/Source`, tree SHA-256
`203706B371B0896468ED590B078E0F265DBDAE17A72D4F7B180FDBB77BAED04E`
(1548 files, buildable, not byte-identical B565).

## Граница приёмки

- Только node: `--mainnet --headless --no-wallet`.
- Запуск: `Node\Beta\start-node.ps1 -ConfigPath <config.json>`.
- Wallet+node, public inbound, public Mainnet reorg, 24h stability — вне Beta.

## Smoke

- Процесс жив; `/v1/health` = `ready`.
- `/v1/status`: `chain_type=main`, `SYNCING_HEADERS`, height 0.
- Свежий full suite (2026-11-09, freeze pair): **PASS — 468 test cases /
  19 309 assertions, exit 0** (ASIO teardown не сработал).
- Targeted: W-06 9/84 PASS, Tor* 5/16 PASS, P2P* 7/110 PASS,
  SyncLogGate 3/5023 PASS, TxHashSet progress 3/13 PASS, RateLimit 3/15 PASS,
  HandShake 2/14 PASS, MessageHeader 1/5 PASS.
- `TorConfig*` в overlay Tests: **PASS — 3 cases / 37 assertions** (×2).
- ACL `.api_secret` live: **PASS** — bad inherited DACL repaired to owner+SYSTEM,
  token unchanged; negative fail-closed (no WRITE_DAC) also observed.
- Overlay full suite: **PASS ×3 — 471/471 cases, 19 340/19 343/19 346 assertions**.
- Исторический full suite: 468/19 303 PASS; новый прогон — 468/19 309 PASS.

## Что вошло в Beta

### P2P

- GP-1: `InboundAdmission` (32 pending / 16 в секунду / 4 на IP), слот
  освобождается на всех путях.
- GP-2: повторный `async_accept` до обработки accepted socket.
- GP-3: header проверяется до body, type/max length, общий 15 s deadline.
- Archive: wrong-genesis reject, unrequested/late/evicted close/no-ban,
  `gcount()` финального chunk, cleanup interrupted transfer, retry/peer switch.

### Rate / логи / статус

- task-72: loopback >500/min → `IgnoreLoopback`, без ban; public >500/min
  сохраняет ban.
- task-66: `SyncLogGate` — один warning на transition/epoch.
- task-67: `ClearTxHashSetProgress()` на failure/abort.

### Wallet / storage / security

- SendHandler: RPC error вместо ложного SENT.
- Wallet encryption: format 1 encrypt-then-MAC, миграция format 0/1 →
  authenticated row-bound format 2, backup/rollback.
- SqliteStore compile defect исправлен.
- DbTool: tip hash check, cleanup partial destination.
- ENOSPC/low-disk: fail-closed, без ложного FULLY_SYNCED.
- `.api_secret`: one-time ACL repair owner-only, токен не перезаписывается.
- Tor: уникальный temp, reject symlink/hardlink, atomic replace.
- PIBD archive serving: default off, Mainnet refusal, opt-in через env.

### Ограничения

- Public Mainnet reorg — NOT VERIFIED; test-chain reorg PASS.
- Direct inbound/firewall — NOT VERIFIED; relay GCP PASS с ограничением source IP.
- External >500/min false-ban — risk подтверждён, live не воспроизведён.
- Sync-burst 5000/min fix — не входит в Beta delivery.
- 24h stability, Linux/FreeBSD/macOS full sync — вне/не завершены.
- ACL repair реального пользовательского профиля, Tor live bridge, wallet+node
  live — NOT VERIFIED.

## RC1

`Node/RC1/GrinNode.exe`
`CD941F325B6E7595E9A777DDA90B486BCAFB0473322CBD95E3FCA5133344FD48`.
RC1 — reference, не финальный Beta.
