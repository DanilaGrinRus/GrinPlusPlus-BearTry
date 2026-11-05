# Grin++ — общественный форк (кандидат Node Beta 1)

Этот репозиторий — общественный форк [GrinPlusPlus](https://github.com/GrinPlusPlus/GrinPlusPlus).
Он **не** является официальным выпуском вышестоящего проекта и не поддерживается
его командой. Форк сосредоточен на **модуле ноды** (GrinNode); кошелёк и UI **не**
входят в приёмку Node Beta.

| | |
|---|---|
| Вышестоящий проект | https://github.com/GrinPlusPlus/GrinPlusPlus |
| Базовая метка | `v1.2.9` |
| Базовый коммит | `a0958a391c30cf630e3e08534fbc6fe9fb194326` (22.10.2024) |
| Лицензия проекта | MIT (см. [LICENSE](LICENSE), копирайт вышестоящего проекта сохранён) |
| Стадия | **Кандидат Node Beta 1** — не стабильный выпуск, не готовность к Mainnet |
| Git-метка | `node-beta.1-candidate` — аннотированный объект `fc5174ec…`, коммит `ccc4f356…` |

## Статус кратко

Полный статус с доказательствами — в [docs/STATUS.ru.md](docs/STATUS.ru.md),
оставшиеся работы — в [docs/ROADMAP.ru.md](docs/ROADMAP.ru.md).

| Область | Статус |
|---|---|
| Нода (Windows 11 x64, MSVC 14.44) | Кандидат собирается, проходит полный набор тестов и завершил живую синхронизацию Floonet до `FULLY_SYNCED` с актуальным индексом после перезапуска (**PASS, в границах**; см. [docs/STATUS.ru.md](docs/STATUS.ru.md)). |
| Чистая пересборка | **PASS** — чистый клон собирается и проходит тот же набор; не побитово идентичен кандидату (метаданные сборки) |
| Mainnet | **НЕ ПРОВЕРЕН** |
| HTTP/RPC API ноды | **ЧАСТИЧНО** — зарегистрированные методы работают локально, полный паритет с Rust не заявлен |
| Кошелёк | **НЕ ГОТОВ** — включён из upstream с частью исправлений, в этот выпуск не принят |
| UI | В этом репозитории отсутствует; **НЕ ГОТОВ** |
| FastInit header checkpoint | **ЭКСПЕРИМЕНТАЛЬНО**, только opt-in |
| PIHD (расширение синхронизации заголовков) | **ПЛАН / ЭКСПЕРИМЕНТ**, по умолчанию выключено |
| Многопировый планировщик PIBD | Реализован и детерминированно протестирован; живой многопировый PIBD подтверждён в объединённом прогоне |
| Автоматический выбор потоков rangeproof | Opt-in (`GRINPP_RANGEPROOF_WORKERS=auto`); по умолчанию 1 |

**Node Beta — это не готовность всего продукта.** PASS на Floonet не означает
PASS на Mainnet. Ни один результат не переносится на другой бинарник: каждое
утверждение в документации привязано к коммиту и SHA исполняемого файла из
[docs/evidence/INDEX.ru.md](docs/evidence/INDEX.ru.md).

## Идентичность кандидата

Финальное дерево исходников и бинарники для этого снимка документации:

| Артефакт | SHA-256 |
|---|---|
| `GrinNode.exe` | `4039086CE339106DE37D7C6A98D54873171458699507DB18E938AFD938C80EFC` |
| `Tests.exe` | `A9594508FBDF5B48AA123A862CBB68B20882AD98BB8AA437A9E9A9C60D4FAC8C` |
| Манифест исходников кандидата (1528 файлов) | `964B2DD8201018136EDFA3B87BFB98924F2A0FCAA508E5117DE2630380BA9627` |

Коммит/метка кандидата опубликованы в истории репозитория
(`node-beta.1-candidate`). Пересборка того же исходного кода не гарантирует
побитово идентичный EXE: результат зависит от инструментов, SDK и путей.
Выполните `sha256sum` для своих артефактов и зафиксируйте их.

## Проверенная платформа

- **Проверено:** Windows 11 x64, Visual Studio 2022 MSVC 14.44, Release,
  `/W4 /WX`, статический CRT. Все результаты PASS в этом репозитории получены
  на этой платформе.
- **Поддерживается исходниками, но не проверено здесь:** Windows 10 x64;
  Linux x64 с GCC. Конфигурация upstream также собирала macOS с GCC; Clang
  отвергается в `CMakeLists.txt`. Результатов выполнения на Windows 10,
  Linux или macOS не заявлено.

## Сборка, тесты, запуск

Предварительные требования, закреплённые зависимости и точные команды — в
[docs/BUILD.ru.md](docs/BUILD.ru.md).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=<prebuilt-sysroot>" `
  -DGRINPP_TESTS=ON -DGRINPP_TOOLS=OFF
cmake --build build --config Release --target GrinNode Tests --parallel

# Запуск headless-ноды Floonet без кошелька
.\bin\Release\Release\GrinNode.exe --floonet --headless --no-wallet
```

Без аргументов нода выбирает **Mainnet** и запускает кошелёк. Для любого теста
используйте изолированный профиль (`--config-path` и `DATA_PATH`) и никогда не
направляйте тест на профиль с реальными средствами.

Пути по умолчанию: профиль `%USERPROFILE%\.GrinPP\<NETWORK>`, конфигурация
`%USERPROFILE%\.GrinPP\<NETWORK>\server_config.json`. Порты REST/P2P по
умолчанию 3413/3414 на Mainnet и 13413/13414 на Floonet.

## Документация

- [docs/STATUS.ru.md](docs/STATUS.ru.md) — что реализовано, проверено и открыто,
  с метками PASS / PARTIAL / FAIL / NOT VERIFIED / PLANNED.
- [docs/ROADMAP.ru.md](docs/ROADMAP.ru.md) — оставшиеся работы и критерии выхода.
- [docs/ARCHITECTURE.ru.md](docs/ARCHITECTURE.ru.md) — устройство ноды,
  синхронизация, хранилище, KernelIndex, модель доверия FastInit и раскладка
  файлов на диске.
- [docs/API.ru.md](docs/API.ru.md) — HTTP/REST и JSON-RPC, аутентификация,
  статусы и известные отличия от Rust Grin 5.5.2.
- [docs/BUILD.ru.md](docs/BUILD.ru.md) — чистая сборка, тесты, запуск, типичные
  ошибки.
- [docs/history/FROM-UPSTREAM-TO-CURRENT.ru.md](docs/history/FROM-UPSTREAM-TO-CURRENT.ru.md)
  — что изменилось после upstream v1.2.9, зачем и какие есть доказательства.
- [docs/evidence/INDEX.ru.md](docs/evidence/INDEX.ru.md) — карта «утверждение →
  доказательство».
- [KNOWN_ISSUES.md](KNOWN_ISSUES.md) — известные ограничения и открытые дефекты.
- [CHANGELOG.md](CHANGELOG.md) — сводка изменений для выпуска.
- [CONTRIBUTING.md](CONTRIBUTING.md) — как участвовать и какие доказательства
  требуются.
- [SECURITY.md](SECURITY.md) — ответственное раскрытие и правила безопасного
  тестирования.
- [docs/HISTORY-RECONSTRUCTION.md](docs/HISTORY-RECONSTRUCTION.md) — как была
  восстановлена история коммитов.

Английский текст является каноническим. Если перевод и английский текст
расходятся, приоритет у английского.

## Тестирование

Полный набор кандидата проходит (`373` тестовых случая, `4982` проверки, exit 0).
Часть проверок требует внешних фикстур или явных тегов и **пропускается**, а не
проходит; точный список — в [docs/BUILD.ru.md](docs/BUILD.ru.md) и
[docs/STATUS.ru.md](docs/STATUS.ru.md).

## Безопасность

Это развивающееся ПО для тестовых сетей. Не используйте его с реальными
средствами. Не публикуйте seed-фразы, приватные ключи, API-секреты
(`.api_secret`) и личные данные профиля. См. [SECURITY.md](SECURITY.md).

## Лицензия и уведомления

MIT — см. [LICENSE](LICENSE). Копирайт upstream сохранён. Встроенные зависимости
и их уведомления перечислены в [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); полные тексты лицензий — в [LICENSES/](LICENSES/README.md).

---

[English version](README.md) · [Deutsche Fassung](README.de.md)
