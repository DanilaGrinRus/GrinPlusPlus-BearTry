# Не проверено и вычеркнуто из Beta acceptance

Дата: 2026-11-09. Документ фиксирует только то, что **не подтверждено** или
**снято с приёмки**. Отсутствие пункта здесь не означает PASS — PASS-пункты
перечислены в `CHANGES_VS_RC1.md` / отчётах evidence.

## A. Вычеркнуто из Beta acceptance

| Пункт | Решение | Причина / что вместо |
|---|---|---|
| Public Mainnet reorg | снят с приёмки | реальное событие нельзя индуцировать; вместо него **test-chain reorg** (`REORG 1–4`, `Reorg Chain`) — TESTNET PASS, см. `TESTCHAIN-REORG-FINAL.md` |
| 24h stability Beta | не входит в Beta acceptance | 24h — отдельный operational VPS-трек; на момент freeze не завершён |
| Linux native build/поставка | не входит в Windows Beta | отдельный Docker/vcpkg трек; на момент freeze image/binary не готовы |
| FreeBSD на GCP | снято с этого трека | `images import --os=freebsd` не поддерживается; raw/qcow2 `images create` отклоняет |
| macOS на GCP | снято | GCP не предоставляет macOS; только GitHub Actions/AWS Mac, вне Beta |
| Wallet+node mode | вычеркнут из Beta boundary | Beta acceptance — node-only (`--no-wallet`); см. `README_NODE_ONLY.md` |

## B. Не проверено (NOT VERIFIED)

### Public inbound / сеть

- **Direct public inbound / firewall traversal** — NOT VERIFIED. Нужен проброс
  порта на роутере или внешний хост.
- Relay-based public ingress через GCP **PASS с ограничением**: public TCP →
  GCP → reverse tunnel → локальный узел; сервер видит `127.0.0.1`, не внешний
  source IP. Полноценный direct inbound не доказан.
  См. `PUBLIC-INBOUND-GCP-FINAL.md`.
- Публичная атака/иностранные хосты (slot exhaustion за пределами loopback) —
  NOT VERIFIED; loopback 8 IP × 4 PASS.

### Stability / VPS

- **24h полный прогон Beta** — NOT VERIFIED. Windows VPS синкается, но 24h не
  истекли; Linux/FreeBSD/macOS не запущены.
- **Windows VPS full sync** — в процессе (`SYNCING_HEADERS`, ~1.6M headers), не
  FULLY_SYNCED на момент freeze.
- **Linux full sync / Docker image** — NOT VERIFIED: vcpkg build ещё идёт,
  `GrinNode`/image не выпущены.
- **FreeBSD** — VM не создана (image import blocker).
- **macOS** — сборка/тесты не запускались (GCP невозможен).

### P2P / rate

- **External false-ban** при >500 входящих/мин — **не воспроизведён live**;
  loopback fixed, внешний peer остаётся под тем же 500/min порогом. Это
  подтверждённый risk, не доказанный Mainnet-сбой.
- **Sync-burst rate fix** (отдельный sync-счётчик 5000/min) — staged/собран
  частично, **не входит в Beta delivery**; external/raw harness не пройден.
- **03b late/evicted sender-side** — receiver-side PASS; отдельный C++ sender
  live не проверен (scripted peer PASS, task-69).
- **03c interrupted live** — receiver-side PASS; sender-side interrupted PASS на
  scripted peer, реальный C++ sender — ограничение.

### Wallet / storage / security

- **ACL repair реального пользовательского Mainnet-профиля** — NOT VERIFIED:
  проверено на свежем Mainnet-профиле и на тестовом, реальный профиль
  пользователя не трогали.
- **No-fill control `FULLY_SYNCED`** — NOT VERIFIED: state install/commit PASS,
  но после sync узел забанил loopback-пир старым rate-лимитом; полный
  FULLY_SYNCED control не получен.
- **Wallet+node live** — NOT VERIFIED (вне Beta boundary).
- **Tor live bridge/obfs4/snowflake + tor daemon** — NOT VERIFIED; проверен
  только atomic replace кода.
- **Mid-sync ENOSPC `FULLY_SYNCED`** — state install PASS; полный no-fill control
  не завершён.
- **DbTool ENOSPC/low-disk** — реальный ENOSPC проверен в storage-треке; CLI
  regression PASS, но полный пользовательский сценарий не исчерпан.

## C. Test-harness ограничение

- Свежий full/targeted прогон `Tests.exe B565…` из Beta 2026-11-09 падает в
  ASIO teardown `0xC0000409` **до summary**; это известно как flaky harness.
  Исторический full suite для этой пары: **468 test cases / 19 303 assertions
  PASS**. Свежий PASS не заявляется.
- RC1 historical suite: 362 tests / 4886 assertions — другой набор/конфигурация,
  напрямую не сравнивать.

## D. Явно не входит в Beta

- Public Mainnet reorg.
- Direct home-NAT inbound.
- Linux/FreeBSD/macOS VPS как часть Windows Beta.
- 24h stability как условие Beta freeze.
- Wallet+node acceptance.

Остальные PASS-пункты и их evidence — в `CHANGES_VS_RC1.md`, `OPTIMIZATIONS.md`
и `evidence/wallet-node-readiness-20261108/`.
