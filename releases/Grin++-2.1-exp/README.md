# Grin++ 2.1 Exp — экспериментальный комплект

Экспериментальная сборка на базе Gplus 2.0.0 + порт **Grin PR #3963 (HEADER_PROOF_HIST)**
и включённый **TX_KERNEL_HASH**. Версия: **Grin++ 2.1.0-exp**.
Это тестовая версия: не для основной поставки, но полностью рабочая с текущей сетью.

## Состав
| Файл | Назначение |
|---|---|
| `GrinNode.exe` | экспериментальная нода 2.1.0-exp (mainnet/floonet) |
| `tor\` | Tor-рантайм; держать рядом с `GrinNode.exe` |
| `grinpp-desktop.exe` | UI 2.1.0-exp (portable) |
| `Grin++_2.1.0-exp_x64-setup.exe` | установщик UI (NSIS) |
| `Start Grin++.cmd` | portable-запуск UI |
| `sources\` | исходники ноды (ветка `exp/header-proof-hist`, коммит `f99d740`) и UI |
| `SHA256SUMS.txt` | контрольные суммы |

## Запуск
1. **Portable**: `Start Grin++.cmd` (UI найдёт ноду рядом, данные в `.GrinPP\`).
2. **Установка**: установить `Grin++_2.1.0-exp_x64-setup.exe`, положить `GrinNode.exe`
   и `tor\` рядом с UI (или в `node\`).
3. В UI → «Нода» → «Запустить»; сеть переключается в шапке.

## Возможности (всё из 2.0 плюс эксперимент)
- Всё, что умеет 2.0: mainnet/floonet, PIBD приём и раздача, кошелёк, платежи,
  история, QR, скрытие баланса, логи, ru/en, управление нодой, выключатель Tor.
- **HEADER_PROOF_HIST (0x100)** — новый бит и сообщения **31/32**:
  - сервер отдаёт исторические header-MMR proof против `anchor.prev_root`
    (fail-closed, лимит 60 запросов/мин на IP, без банов);
  - клиентский верификатор: membership + PoW + WTEMA-триплет h-2/h-1/h (post-HF4,
    pre-HF4 — явный отказ);
  - live-проверка: `caps=0x15F`, `LIVE HEADER-PROOF: PASS` (3/3 proof).
- **TX_KERNEL_HASH (0x08)** объявляется в handshake.
- PIBD_HIST (0x10/0x40) — как в 2.0, включается при готовом архиве.

## Проверки
- `*HeaderProof*`: **10 кейсов / 329 assertions PASS** (включая multi-peak 3/5/7/11/13/19).
- Полный набор до фикса: 502 кейса / 19 721 assertion PASS; после фикса прогнаны
  целевые тесты + live-пробник (`tools/header_proof_probe`).
- Live: mainnet `FULLY_SYNCED`, PIBD-архив 4 099 680, caps `0x15F`, header-proof PASS.

## Ограничения (эксперимент)
- Live-интероп с Rust-пиром, собранным из PR #3963, не проверялся (нет такого бинаря).
- WTEMA реализована inline по триплету; pre-HF4 (DMA/dual-PoW) не поддерживается.
- Бит `0x100` — вне 8-битных масок: клиенты, обрезающие caps до 0xFF, увидят `0x5F`.
- Для входящих P2P нужен открытый порт 3414.

SHA-256 всех файлов — в `SHA256SUMS.txt`.