# Индекс доказательств (краткая русская версия)

Канонический и полный индекс — [INDEX.md](INDEX.md). Здесь — краткая карта для
русскоязычных читателей; при расхождении приоритет у английского файла.
Оригиналы, SHA-256 и правила редактирования — в [ORIGINALS.md](ORIGINALS.md).

## Среда

Windows 11 x64, Visual Studio 2022 MSVC 14.44, Release `/W4 /WX`, статический
CRT. Кандидат: `node-beta.1-candidate`, `GrinNode.exe`
`4039086C…`, `Tests.exe` `A9594508…`, манифест исходников
`964B2DD8…` (1528 файлов).

## Публичные файлы (`docs/evidence/files/`)

| Файл | Что это |
|---|---|
| `tests-full-final.log` | Полный набор: 373 случая / 4982 проверки, exit 0 |
| `target-final.json` | Целевые: orphan 17, faults 81, pibd install 25, PibdScheduler 68/9, BlockPipe 41/6 |
| `tests-fullsweep.log` | FullSweep: 6 проверок PASS с предупреждением о пустой фикстуре |
| `nrd-kill-manifest.json` | Манифест принудительного kill/recovery |
| `nrd-kill-child.stdout.log`, `nrd-kill-recovery.stdout.log` | Вывод убитого процесса и recovery (241 проверка, exit 0) |
| `combined-live-floonet-2026-11-05.md` | Очищенная выжимка live-прогона Floonet на объединённой сборке `16C24DB4…` |
| `candidate-source-manifest-final.sha256` | Финальный манифест исходников |

## Ключевые утверждения

| ID | Утверждение | Статус |
|---|---|---|
| E-01 | Полный набор на кандидате | **PASS** |
| E-02 | Целевые index/scheduler/block-pipe тесты | **PASS** |
| E-04 | Kill/recovery NRD | **PASS** (принудительный kill, не потеря питания) |
| E-06 | Локальная Release-сборка кандидата | **PASS (локально)** |
| E-07 | Живой Floonet PASS на `16C24DB4…` | **PASS (в границах бинарника)** |
| E-08 | Живой прогон кандидата `4039086C…` | **В ПРОЦЕССЕ / NOT VERIFIED** |
| E-09 | Mainnet | **NOT VERIFIED** |
| E-11/E-12 | KernelIndex orphan/faults/install | **PASS** (локальные тесты) |
| E-14 | Реальный RocksDB failure / power loss | **NOT VERIFIED** |
| E-18 | Политика многопирового PIBD | **PASS** (детерминированные тесты) |
| E-19 | Локальная матрица API (27 проверок) | **PASS** на промежуточных бинарниках; на финальном не повторялась |
| E-20 | 4 owner-метода управления цепью | **NOT IMPLEMENTED** |
| E-22 | FastInit | **EXPERIMENTAL / NOT VERIFIED** |
| E-23 | PIHD | **PLANNED / EXPERIMENTAL** |
| E-24 | Wallet/UI | **NOT READY** |

## Правила

- Пропущенный тест, отсутствующая фикстура, таймаут или mock — не PASS.
- Результат принадлежит бинарнику и сценарию; на другой SHA не переносится.
- Устаревший `source-manifest.sha256` — только история происхождения; актуален
  финальный манифест.
