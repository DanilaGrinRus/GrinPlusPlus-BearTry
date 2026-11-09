# Оптимизации Beta (control-plane / resource / logging)

Дата: 2026-11-09. Документ описывает только оптимизации/ограничения ресурсов,
которые вошли в Beta. Это **не** заявление об ускорении консенсуса: отдельных
производительных бенчмарков RC1 vs Beta не проводилось.

## 1. P2P admission: ограничение pre-handshake ресурсов

- `InboundAdmission`: **32 pending**, **16 подключений/с**, **4 на IP**.
- Слот освобождается ровно один раз на success и всех failure-путях
  (`Connection::NotifyHandshakeComplete`).
- Эффект: ограниченное число неаутентифицированных сокетов, защита listener от
  исчерпания слотами.
- Evidence: `P2P-GP1-GP2-FINAL.md`; live slot exhaustion: 8 IP × 4, 33-й закрыт,
  все 32 освобождены за ~15 s.

## 2. Handshake: ранняя проверка и deadline

- Header читается первым; тип и максимальный размер body проверяются **до**
  чтения body.
- Общий 15 s deadline на header+body, чтение через `available()`/`read_some()`.
- Эффект: медленный/неправильный peer не занимает слот до 18.6 s; нет лишнего
  чтения большого body.
- Evidence: live no-data 15.015 s, partial header 15.000 s, wrong type/size —
  мгновенное закрытие (`P2P-GP3-FINAL.md`).

## 3. Rate policy: sync burst и loopback

- Обычный public abuse-порог `>500 msg/min` сохранён.
- Для **loopback** `>500/min` — только throttled warning, без 3 h ban.
- Для remote во время активного sync: отдельный bounded allowance
  `MAX_SYNC_RECEIVED_PER_MINUTE = 5000`, при non-sync ≤500; иначе Ban.
- Эффект: здоровый локальный/быстрый catch-up не теряет peer; public DoS-защита
  не отключена.
- Evidence: task-72 (887 msgs/62 s — Banning нет, peer Healthy); task-64/69.

## 4. Логирование: подавление flood и throttle

- `SyncLogGate`: один warning на transition/request epoch для
  peer-lost/sync-failed/120 s no-progress/120 min timeout.
- Loopback/sync rate warnings throttled до одного раза в 60 s.
- Эффект: существенно меньше I/O и размера `Node.log` без потери переходов.
- Evidence: task-66; flood **15 306 строк / 1.66 MB за 6.5 min → 2 строки за ~90 s**.

## 5. Статус: очистка stale progress

- `ClearTxHashSetProgress()` на failure/abort очищает
  `download_size/downloaded/processing_status`.
- Эффект: `/v1/status` не показывает 64 MiB после прерванной передачи; меньше
  ложных тревог оператора.
- Evidence: task-67; после RST `download_size=0`, `downloaded=0`.

## 6. Tor config: атомарная запись

- Уникальный temp рядом с torrc, reject symlink/hardlink, atomic replace
  (`MoveFileExW`/POSIX `rename`).
- Эффект: нет temp в CWD и окна `remove+rename`; меньше риска повреждения
  конфигурации и лишних I/O-ошибок.
- Evidence: `TOR-FINAL.md`.

## 7. PIBD archive serving: default OFF

- `PIBD_ENABLED=false`/archive serving default disabled; Mainnet refusal;
  opt-in только через env.
- Эффект: по умолчанию не тратит диск/трафик и не расширяет поверхность атаки.
- Evidence: `STORAGE-FINAL.md`, `pibd-serving-policy.json`.

## 8. Build/runtime packaging

- Windows: pinned sysroot, static CRT (`/MT`), serial MSBuild;
  node-only launcher (`--mainnet --headless --no-wallet`) исключает не
  принятый wallet+node режим.
- Linux: Docker/vcpkg build с фиксированными портами — в работе; в Beta
  Windows-поставку не входит.
- Это packaging/сборочная оптимизация, не измеренный runtime speedup.

## Ограничения

- Многопоточность/CPU-профили специально не оптимизировались и не
  бенчмаркались в этом цикле.
- Внешний rate-limit burst-fix (sync-счётчик) **не включён** в Beta delivery;
  delivery — `B565612B…`, см. `UNVERIFIED_AND_REMOVED.md`.
