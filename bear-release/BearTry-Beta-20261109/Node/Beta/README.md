# Gplus Beta — frozen node-only candidate

Статус: **FROZEN** (2026-11-09).
Это финальный Beta-кандидат; RC1 лежит отдельно в `..\RC1`.

## Артефакты

| Файл | Размер | SHA-256 |
|---|---:|---|
| `GrinNode.exe` | 8 462 336 | `B565612BCA31D9EFE4117FC37FD2C7680ED8B84D9830E47591EFA3C28BA3D64D` |
| `Tests.exe` | 11 612 672 | `2B5BD9853979ED7AEFB4B2C3E6D9B27B6BB397E6E816CE2D52FC9E2E74B25DF3` |
| `GrinDbTool.exe` | 3 891 712 | `8C0113D2E11A3655DDE2372515974E6B7D832076CC5229C82706616B89F00656` |

Полный manifest: `FREEZE_SHA256SUMS.json`.

## Запуск (принятый Beta-режим)

```powershell
.\start-node.ps1 -ConfigPath .\config.json
```

Launcher всегда передаёт:

```text
--mainnet --headless --no-wallet
```

Wallet+node — отдельный трек, не часть Beta acceptance.

## Smoke (2026-11-09)

Запуск именно из `C:\DHW\Gplus\Beta`:

- процесс жив;
- `/v1/health` = `{"status":"ready"}`;
- `/v1/status`: `chain_type=main`, `sync_status=SYNCING_HEADERS`, height 0;
- создан `profile\LOGS\Node.log`.

**Smoke PASS.**

## Тесты

- Исторический full suite для этой пары: **468 test cases / 19 303 assertions PASS**
  (task-72, до freeze).
- Повторный full/targeted запуск из Beta 2026-11-09 падает в известный
  ASIO teardown `0xC0000409` до вывода summary (`TESTS-SMOKE.log`,
  `TESTS-P2P-TARGETED.log`). Это flaky harness, не FAIL теста и не отличие
  binary; для freeze используется исторический PASS + smoke.

## Ограничения

- Public inbound/firewall и public Mainnet reorg не входят в Beta acceptance.
- Windows VPS/24h — отдельный operational трек.
- Freeze относится к тройке `.exe` и manifest; `profile/` и логи в этой папке
  не являются частью поставки.

## Документы изменений

- [CHANGES_VS_RC1.md](CHANGES_VS_RC1.md)
- [OPTIMIZATIONS.md](OPTIMIZATIONS.md)
- [UNVERIFIED_AND_REMOVED.md](UNVERIFIED_AND_REMOVED.md)

## Исходники

- `Source/` — snapshot исходников ноды (src/include/tests/cmake/deps/vcpkg/tools).
- `Source/SOURCE_PROVENANCE.json` — commit/dirty/оговорка о том, что бинарник собран из рабочего source-build до очистки work.
