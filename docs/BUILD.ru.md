# Сборка, тесты и запуск

Это каноническое руководство по сборке. Оно относится к дереву с меткой
`node-beta.1-candidate`. Корневой [`BUILD.md`](../BUILD.md) — указатель на этот
файл.

## 1. Предварительные требования

**Проверенный инструментарий (все результаты PASS в этом репозитории):**

- Windows 11 x64.
- Visual Studio 2022 MSVC 14.44 (v143) с Windows SDK 10/11.
- CMake 3.20 или новее (проект объявляет 3.8, но проверялся генератор VS 2022).
- PowerShell 7 или Windows PowerShell 5.1.
- Python 3.10+ — только для helper-скрипта sysroot (необязательно).
- Не менее 16 GB RAM для сборки; сама нода на обычном старте занимает примерно
  `2,4–2,5 GB` рабочего набора и до `5,7 GB` при полной перестройке индекса
  ядер на состоянии ~3,36 млн ядер.

**Поддерживается исходниками, но не проверено в этом форке:** Windows 10 x64 и
Linux x64 с GCC. `CMakeLists.txt` отвергает Clang, поэтому для macOS нужен GCC;
эта платформа здесь также не проверена. Не представляйте бинарники для
Windows 10, Linux или macOS как протестированные.

## 2. Зависимости

Проект линкуется со статическим CRT (`/MT`) и ожидает готовые библиотеки с
CMake package config. Есть два пути.

### 2.1 Готовый sysroot (путь, использованный для сборки кандидата)

Кандидат собран против локального sysroot Windows x64, собранного из
закреплённых ниже ревизий. Готовые бинарники **не** коммитятся в этот
репозиторий (около 162 MB). Возьмите sysroot из релизного пакета или соберите
свой с теми же пинами.

```powershell
# Портативный helper (tools/deps/build_sysroot.py) публикуется как проверенная отправная точка.
python tools/deps/build_sysroot.py --dry-run   # сначала посмотреть
python tools/deps/build_sysroot.py
# Затем передайте префикс:
#   -DCMAKE_PREFIX_PATH=<абсолютный путь к sysroot>
```

**Статус пересборки зависимостей:** портативный helper проверен и запускался в
dry-run, но **не выполнялся end-to-end** во время подготовки публичного форка.
Опубликованный sysroot получен прежним локальным pipeline из тех же закреплённых
версий. Считать «пересобрать sysroot этим helper» — **NOT VERIFIED**, пока кто-то
не воспроизведёт его и не сверит хеш манифеста.

Если helper отсутствует, воспроизведите пины из таблицы ниже. Эталонный sysroot
кандидата содержит 746 файлов и 162 287 187 байт; SHA-256 его манифеста —
`82FBF0F7F2D5E5BFFB8C049479E677A10F47177E6A55459269960EAD54EC5C53`.
Используйте этот манифест как якорь целостности при получении готового бандла от
третьей стороны.

### 2.2 vcpkg (путь upstream)

Upstream использовал vcpkg commit
`6185aa76504a5025f36754324abf307cc776f3da` (2020.06) с overlay-портами и
triplet'ами в `vcpkg/` и списком пакетов в `vcpkg/packages.txt`. Этот путь
старше VS 2022; он документирован для совместимости, но **не проверен** с
инструментарием VS 2022, использованным здесь.

### 2.3 Закреплённые ревизии

| Зависимость | Ревизия |
|---|---|
| zlib / minizip | 1.2.11 |
| libsodium | 1.0.18 |
| secp256k1-zkp | `5b39fbf7f1e16e126404791923511723c4c0876d` |
| RocksDB | 6.6.4 (`551a110918493a19d11243f53408b97485de1411`) |
| civetweb | `2c1caa6e690bfe3b435a10c372ab2dcd14b872e8` |
| CRoaring | 0.2.66 |
| fmt | 6.2.1 |
| asio | 1.12.2 |
| mio | `cafa31360fee8866be89b4c602d8b9a7a18dbf5e` |
| Catch2 | 2.12.1 |
| vcpkg (пин upstream) | `6185aa76504a5025f36754324abf307cc776f3da` |

Встроены в `deps/` и собираются из этого дерева: bitcoin, reproc, stduuid,
cppcodec, uint128, caches, scheduler, spdlog, ghc::filesystem, jsoncpp,
заголовки Catch2, scrypt, chachapoly, sqlite3, rfc6234, sha3, klib. Уведомления
— в [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

## 3. Получение исходников

```powershell
git clone <repository-url> grinpp
cd grinpp
git checkout node-beta.1-candidate
```

Обязательных submodule нет; `deps/` вендорится. Не копируйте в дерево исходников
профили, `bin/`, логи или каталоги сборки с конкретной машины.

## 4. Настройка и сборка

```powershell
$sysroot = "C:\path\to\sysroot"   # абсолютный путь; должен содержать CMake configs
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_PREFIX_PATH=$sysroot" `
  -DGRINPP_TESTS=ON `
  -DGRINPP_TOOLS=OFF
cmake --build build --config Release --target GrinNode Tests --parallel 12
```

Ожидаемое расположение результатов (проект пишет в дерево исходников):

```text
bin\Release\Release\GrinNode.exe
bin\Release\Release\Tests.exe
```

`-DGRINPP_TOOLS=OFF` собирает только ноду и тесты. `slate_tool` и `tx_verifier`
— вспомогательные и для релиза ноды не нужны.

Если MSBuild падает с ошибками file tracker или именованных каналов в
ограниченной среде, собирайте без параллельных узлов MSBuild (`--parallel 1`) и
держите временный каталог коротким (`$env:TMP = $env:TEMP = "C:\tmp"`).

Результаты сборки не гарантированно побитово идентичны на разных машинах:
версия компилятора, SDK, пути и порядок линковки меняют бинарник. Записывайте
SHA-256 своих артефактов.

## 5. Тесты

Всегда запускайте тесты в изолированном домашнем каталоге. Тестовые бинарники
пишут профили в `%HOME%\.GrinPP`; запуск на реальном профиле может его изменить.

```powershell
$testRoot = Join-Path $PWD ("test-run-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force (Join-Path $testRoot "temp") | Out-Null
$rootPath = [IO.Path]::GetPathRoot($testRoot).TrimEnd('\')
$env:HOMEDRIVE = $rootPath
$env:HOMEPATH  = $testRoot.Substring($rootPath.Length)
$env:TEMP = $env:TMP = Join-Path $testRoot "temp"

& .\bin\Release\Release\Tests.exe "~[.]"
```

На кандидате выводится:

```text
All tests passed (4982 assertions in 373 test cases)
```

Скрытые теги (`[.]`) исключаются `~[.]`; запускайте их явно, когда есть фикстуры:

```powershell
.\bin\Release\Release\Tests.exe "[orphan]"
.\bin\Release\Release\Tests.exe "[fault]"
.\bin\Release\Release\Tests.exe "[pibd]"
.\bin\Release\Release\Tests.exe "PibdScheduler*"
.\bin\Release\Release\Tests.exe "BlockPipe*"
.\bin\Release\Release\Tests.exe "REORG*"
.\bin\Release\Release\Tests.exe "PibdSegmentService::FullSweepPpCkpt"
```

Примечания:

- Внешние PIBD/live-векторы не поставляются. Если фикстура отсутствует,
  проверка **пропускается** — это не PASS. Тест `FullSweepPpCkpt` может пройти
  с пустым sweep (`bitmap 0/7, output 0/1678, …`) и предупреждением; это не
  проверка заполненного sweep.
- Harness восстановления после убийства процесса запускается через
  `tools/audit/kernel-index-crash.ps1` и требует `Tests.exe` из того же дерева.
  Он принудительно убивает процесс в выбранной точке транзакции; это не проверка
  потери питания.
- Никогда не запускайте два тестовых бинарника на одном профиле или порту.
  Используйте уникальные порты и уникальные домашние каталоги для каждого
  прогона.

## 6. Запуск ноды

```powershell
# Headless-нода Floonet без кошелька, профиль по умолчанию
.\bin\Release\Release\GrinNode.exe --floonet --headless --no-wallet
```

Полезные опции:

| Опция | Значение |
|---|---|
| `--floonet` | Floonet вместо Mainnet |
| `--automated-testing` | Тестовая цепь (не для сетевого тестирования) |
| `--headless` | Без интерактивного консольного дисплея |
| `--no-wallet` | Только нода |
| `--config-path <file>` | Использовать конкретный `server_config.json` |
| `--shared-node host:port` | Использовать удалённую ноду (режим клиента) |

Пути и порты по умолчанию:

| Пункт | Mainnet | Floonet |
|---|---|---|
| Профиль | `%USERPROFILE%\.GrinPP\MAINNET` | `%USERPROFILE%\.GrinPP\FLOONET` |
| Конфигурация | `…\server_config.json` | `…\server_config.json` |
| REST API | `3413` | `13413` |
| P2P | `3414` | `13414` |
| Данные / логи | `<profile>\NODE`, `<profile>\LOGS` | то же |

Для изолированного запуска создайте конфигурацию с явным `DATA_PATH` и
отдельными портами, затем запустите с `--config-path`. Пример:

```json
{
  "DATA_PATH": "D:\\grinpp-test\\floonet",
  "P2P": { "MIN_PEERS": 3, "MAX_PEERS": 20, "P2P_PORT": 23414 },
  "SERVER": { "REST_API_PORT": 23413 },
  "LOGGER": { "LOG_LEVEL": "DEBUG" }
}
```

API-секрет создаётся при первом запуске в `<DATA_PATH>\.api_secret` (64
hex-символа, DACL только для владельца). Никогда не копируйте его в логи,
отчёты или коммиты. Диагностические логи пишутся рядом с исполняемым файлом в
`logs\`; если каталог недоступен для записи, нода откатывается к
`<DATA_PATH>\LOGS\node-diagnostics`.

Типичные ошибки:

| Симптом | Причина / действие |
|---|---|
| `Could not find a package configuration file provided by …` | `CMAKE_PREFIX_PATH` не указывает на sysroot со всеми закреплёнными configs. |
| `could not find any instance of Visual Studio` | Не установлены VS 2022 Build Tools или workload C++. |
| Нода завершается с ошибкой bind/`address in use` | Порты REST/P2P заняты (другой нодой или тестом). Измените `REST_API_PORT`/`P2P_PORT`. |
| Два экземпляра на одном `DATA_PATH` | Блокировка RocksDB. Не делите профиль между экземплярами; поведение профильной блокировки проверено не полностью. |
| Старт «зависает» на минуты | Полная перестройка индекса ядер и проверка заголовков — часть дизайна; см. [ARCHITECTURE.ru.md](ARCHITECTURE.ru.md). Не убивайте процесс, если не принимаете rebuild. |
| `GET /v1/status` сразу после старта возвращает 404 | Endpoint status регистрируется позже `/v1/health`; для живости процесса используйте health. |
| Owner RPC возвращает `-32001` / REST 401 | Нет секрета или он неверен, либо `.api_secret` не owner-only. Исправьте ACL, а не ослабляйте проверку. |
| Предупреждения о пропущенных тестах | Внешние фикстуры отсутствуют. Пропущенная проверка — NOT VERIFIED, не PASS. |

## 7. Статус проверки сборки

| Пункт | Статус |
|---|---|
| Release-сборка кандидата `/W4 /WX` | **PASS** (локальный лог сборки) |
| Чистая конфигурация из клона | **PASS** (независимая проверка релиза) |
| Чистая сборка из клона и тесты | **В ПРОЦЕССЕ** (проверка релиза; на момент снимка шла компиляция) |
| CI workflow подготовлен | **ДА** (`.github/workflows/ci.yml`) |
| CI выполнен на GitHub Actions | **НЕТ** — hosted CI не является доказательством для этого снимка |
| Сборка/запуск Windows 10 x64 | **NOT VERIFIED** (поддерживается исходниками) |
| Сборка Linux/macOS | **NOT VERIFIED** |
| EXE импортирует только системные DLL на проверенной машине | **PASS** (проверка релиза); запуск с сетевой папки NOT VERIFIED |

---

[English version](BUILD.md) · [Deutsche Fassung](BUILD.de.md) ·
[К README](../README.ru.md)
