# Изменения и исправленные ошибки: RC1 → Beta

Дата: 2026-11-09. База сравнения: RC1 `GrinNode.exe`
`CD941F325B6E7595E9A777DDA90B486BCAFB0473322CBD95E3FCA5133344FD48`.
Beta: `GrinNode.exe`
`B565612BCA31D9EFE4117FC37FD2C7680ED8B84D9830E47591EFA3C28BA3D64D`.

> Краткое доказательство — только уже записанные отчёты/тесты/логи. Полный
> свежий full-suite после freeze не заявляется: повторный запуск упирается в
> известный ASIO teardown (`0xC0000409`), см. `UNVERIFIED_AND_REMOVED.md`.
> Исторический full suite для Beta-пары: **468 test cases / 19 303 assertions PASS**.

## 1. P2P: pre-handshake admission и accept

| Проблема в RC1 | Исправление в Beta | Доказательство |
|---|---|---|
| GP-1: лимит pre-handshake слотов не был подключён в `Seeder::Accept` в post-9E40 деревьях — входящие могли занимать слоты без ограничения | `InboundAdmission::TryAcquire(peerIp)`; 32 pending / 16 в секунду / 4 на IP; слот освобождается на success и всех failure-путях | `evidence/.../P2P-GP1-GP2-FINAL.md`; live slot exhaustion 8 IP × 4, 33-й закрыт, все 32 освобождены за ~15 s |
| GP-2: исключение до повторного `async_accept` могло остановить listener | `async_accept` re-arm до обработки accepted socket; обработка в try/catch | `P2P-GP1-GP2-FINAL.md`; live 8 RST + валидный Hand |
| 04: accept failure/re-arm | повторный accept после RST/ошибок | `p2p-task62/.../06-accept-rearm/`; **PASS** |

## 2. P2P: handshake

| Проблема/изменение | Beta | Доказательство |
|---|---|---|
| GP-3: медленный body/чужой тип удерживал pre-handshake соединение до 18.6 s | header проверяется до body; type + max length; общий 15 s deadline через `available()`/`read_some()` | `P2P-GP3-FINAL.md`; live: no-data 15.015 s, partial header 15.000 s, wrong type/size — мгновенно |
| 03a: wrong-genesis | оба направления reject, без ban и загрязнения состояния | `p2p-task62`/`03-wrong-genesis/`; **PASS** |
| 03b: unrequested archive | close/no-ban, identity history | `p2p-task62`; live **PASS** |
| 03b: late/evicted | late/evicted классифицируются, close/no-ban | task-64/69; live **PASS** |
| 03c: final partial chunk | `gcount()`; реальный архив 1 799 843 634 B, финальный chunk 94 002 B | task-69; **PASS** |
| 03c: interrupted transfer | cleanup temp, no-ban, retry re-arm | task-64/69; **PASS** |
| 06: retry/peer switch | 30 s guard, 120 s no-progress rotation, bounded retries | task-64; live **PASS** |

## 3. Rate limiting и логирование

| Проблема | Beta | Доказательство |
|---|---|---|
| Fast loopback catch-up банил здорового peer на 3 h | loopback >500/min → `IgnoreLoopback`, без ban; public >500/min сохранён | task-72; live 887 msgs/62 s: Banning нет, peer Healthy |
| Log flood `Sync peer no longer connected.` (15 306 строк / 1.66 MB за 6.5 min) | один warning на transition/epoch; throttling | task-66; live ≤2 строки за ~90 s |
| Stale `state.download_size` после failed transfer (64 MiB) | `ClearTxHashSetProgress()` на failure/abort | task-67; live `download_size=0`, `downloaded=0` |

## 4. Wallet / DbTool / storage

| Проблема | Beta | Доказательство |
|---|---|---|
| `SendHandler` возвращал legacy SENT при ошибке broadcast/finalization | RPC error вместо ложного успеха | `W1-FINAL.md`; `Test_Send` |
| Wallet encryption: старый format 0 без MAC; новые записи EtM | format 1 encrypt-then-MAC; полная миграция format 0/1 → authenticated row-bound format 2, backup + rollback | `TOR`/`W1`/`STORAGE-FINAL.md`; targeted W-06, full suite |
| `SqliteStore::OpenWallet` compile defect (`userDBPath`/`seedFile`) | исправлено; clean full build | `W1-FINAL.md` |
| DbTool checkpoint/partial copy | проверка tip hash в confirmed/candidate, cleanup partial destination | DbTool evidence/CLI regression |
| Low-disk/ENOSPC | fail-closed, без ложного FULLY_SYNCED | `STORAGE-FINAL.md`; 64 MiB VHD, реальный ENOSPC |
| Mid-sync/state-write ENOSPC | header + state install при заполнении тома fail-closed | `STORAGE-MIDSYNC-FINAL.md`; state install PASS, FULLY_SYNCED ограничен rate-баном |
| PIBD archive serving | default disabled, Mainnet refusal, opt-in только env | `pibd-serving-policy.json`; targeted tests |

## 5. Security / API / config

| Проблема | Beta | Доказательство |
|---|---|---|
| `.api_secret` с плохим owner/DACL ломал PRIVATE API | one-time ACL repair (owner + protected DACL), токен не перезаписывается | `API-SECRET-ACL-FINAL.md`; private API 200 после поломки DACL |
| Tor `temp.torrc` в CWD + `remove+rename` race | уникальный temp рядом, symlink/hardlink reject, atomic replace (`MoveFileExW`/`rename`) | `TOR-FINAL.md` |
| Wallet+node default | Beta launcher всегда `--mainnet --headless --no-wallet` | `README_NODE_ONLY.md`, `start-node.ps1` |

## 6. Consensus/tests

- Test-chain reorg (`REORG 1–4`, `Reorg Chain`) входит в прошедший full suite;
  публичный Mainnet reorg **не заявляется** — см. `TESTCHAIN-REORG-FINAL.md`.
- Исторический full suite Beta-пары: **468/19 303 PASS**. RC1 historical:
  362/4886 (иной набор/конфигурация; напрямую не сравнивать как покрытие).

## Итог

Beta содержит исправления P2P admission/handshake/archive, rate/logging,
wallet/DbTool/storage, ACL/Tor и node-only boundary. Все пункты выше имеют
отдельный evidence-файл; непроверенное и вычеркнутое — в
`UNVERIFIED_AND_REMOVED.md`.
