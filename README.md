# Gplus / Grin++ — BearTry releases

Независимый форк Grin++ (Gplus): C++-нода + десктопный UI на Tauri/React.
Ветка `main` — **витрина релизов**; полные portable-комплекты лежат в GitHub Releases.

> English: independent Grin++ fork. `main` is the release showcase; full portable kits
> (node + Tor + UI + installer + sources) are attached to GitHub Releases.

## Релизы

| Релиз | Что это | Скачать |
|---|---|---|
| **Grin++ 2.0.0** | стабильная нода+кошелёк+UI; PIBD приём/раздача, mainnet/floonet | [v2.0.0](https://github.com/DanilaGrinRus/GrinPlusPlus-BearTry/releases/tag/v2.0.0) |
| **Grin++ 2.1.0-exp** | экспериментальная: всё из 2.0 + `HEADER_PROOF_HIST (0x100)` (Grin PR #3963, msgs 31/32) | [v2.1.0-exp](https://github.com/DanilaGrinRus/GrinPlusPlus-BearTry/releases/tag/v2.1.0-exp) |
| Node Beta (старое) | предыдущая node-only Beta, сохранена в ветке `beta` | ветка [`beta`](https://github.com/DanilaGrinRus/GrinPlusPlus-BearTry/tree/beta) |

В каждом релизе (zip-ассет): `GrinNode.exe`, `tor\`, `grinpp-desktop.exe`,
NSIS-установщик UI, `Start Grin++.cmd`, README, исходники и `SHA256SUMS.txt`. Полные контрольные суммы комплекта лежат внутри zip; `SHA256SUMS.txt` в папках репозитория покрывает файлы, которые лежат в git.

## Быстрый старт

1. Скачать zip нужного релиза из **Releases** и распаковать.
2. Запустить `Start Grin++.cmd` — UI найдёт `GrinNode.exe` рядом, данные будут в `.GrinPP\`.
   Либо установить UI через `Grin++_*_x64-setup.exe` и положить `GrinNode.exe` + `tor\` рядом.
3. В UI: раздел **«Нода»** → «Запустить»; сеть (Mainnet/Floonet) — в шапке.
4. Кошелёк: «Создать кошелёк» или «Восстановить кошелёк».

Порты по умолчанию: P2P **3414**, REST **3413**, wallet API **3421** (loopback).

## Что умеют сборки

| Возможность | 2.0.0 | 2.1.0-exp |
|---|---|---|
| mainnet / floonet, синк заголовков и блоков | ✅ | ✅ |
| PIBD приём (0x10) и раздача своего архива (0x40) | ✅ | ✅ |
| Раздача PIBD на mainnet только с явным opt-in | ✅ | ✅ |
| `TX_KERNEL_HASH (0x08)` | ✅ | ✅ |
| `HEADER_PROOF_HIST (0x100)`, msgs 31/32, header-MMR proof | — | ✅ |
| Кошелёк: создание/восстановление 12/18/24, отправка/приём, QR | ✅ | ✅ |
| История, фильтры, скрытие баланса, статус с прогрессом/ETA | ✅ | ✅ |
| Управление нодой из UI: Запустить/Стоп/Перезапустить, смена сети | ✅ | ✅ |
| Выключатель Tor (`TOR.ENABLE_TOR`) | ✅ | ✅ |
| Логи по модулям + таймлайн этапов, ru/en | ✅ | ✅ |

## Исходники

- `releases/Grin++-2.0/sources/` — снимок ноды (ветка `master`, `302616f` + Tor-выключатель)
  и UI, из которых собран комплект.
- `releases/Grin++-2.1-exp/sources/` — экспериментальная нода (ветка `exp/header-proof-hist`,
  коммит `f99d740`) и UI 2.1.0-exp.
- Сборка: `BUILD.md` внутри архива исходников ноды.

## Проверки и ограничения

- 2.0.0: полный набор тестов **493 кейса / 19 498 assertions PASS** (до Tor-выключателя);
  после него — сборка и live-проверка Tor on/off, кошелёк и UI.
- 2.1-exp: `*HeaderProof*` **10 кейсов / 329 assertions PASS**; live `caps=0x15F`,
  `LIVE HEADER-PROOF: PASS`; PIBD live PASS на mainnet.
- Ограничения 2.1-exp: экспериментальный статус, нет live-интеропа с Rust-пиром PR #3963,
  pre-HF4 не поддерживается, бит `0x100` теряется у клиентов с 8-битной маской caps.
- Windows x64; для входящих P2P нужен открытый порт 3414 (NAT/firewall).
- В репозиторий и комплекты не входят seed-фразы, `.api_secret`, кошельки и данные сети.

## Структура репозитория

```text
README.md
releases/
  Grin++-2.0/      README, SHA256SUMS, установщик, sources/
  Grin++-2.1-exp/  README, SHA256SUMS, установщик, sources/
```
## P2P: входящие соединения

Для обычного запуска ничего не нужно — нода работает исходящими соединениями.
Если нужна публичная нода/раздача PIBD незнакомым пирам: проброс TCP 3414 на роутере +
правило Windows Firewall. Памятка: [`docs/P2P.md`](docs/P2P.md),
скрипт: [`docs/enable-p2p-firewall.cmd`](docs/enable-p2p-firewall.cmd)
(запускать от администратора). Те же файлы приложены к каждому релизу.