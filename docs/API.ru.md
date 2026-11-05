# HTTP и JSON-RPC API ноды

Этот документ описывает API ноды дерева `node-beta.1-candidate`. Источник
истины — код:

- REST `/v1`: `src/Server/Node/NodeRestServer.cpp` и `src/Server/Node/API/*.cpp`.
- Регистрация JSON-RPC foreign/owner: `src/API/Node/NodeServer.cpp` и
  `src/API/Node/Handlers/*.h`.
- Контроль доступа: `include/Net/Servers/RPC/ApiAccess.h`,
  `src/Net/Servers/RPC/ApiAccess.cpp`, `include/Net/Servers/RPC/RPCServer.h`.

Статусы:

- **IMPLEMENTED** — обработчик существует и покрыт автотестами и/или локальными
  HTTP-проверками.
- **PARTIAL** — реализовано, но схема, матрица ошибок, права или live-фазы
  полностью не сверены с эталоном.
- **NOT IMPLEMENTED** — обработчик не зарегистрирован; фиктивного успеха нет.

Эталон для сравнения — **Rust Grin 5.5.2** (`reference/grin-5.5.2`,
`api/src/foreign_rpc.rs`, `api/src/owner_rpc.rs`). Форк намеренно добавляет
C++-расширения; отличие само по себе не является ошибкой, но должно быть
задокументировано для потребителей.

## Серверы и аутентификация

REST и JSON-RPC обслуживает один локальный listener на
`SERVER.REST_API_PORT` (по умолчанию `3413` Mainnet, `13413` Floonet).
JSON-RPC endpoints — `POST /v2/foreign` и `POST /v2/owner`; тело — JSON-RPC 2.0
(`{"jsonrpc":"2.0","method":"…","params":[…],"id":1}`). REST отвечает обычным
JSON (не JSON-RPC).

При первом запуске нода создаёт 64-hex-секрет в `<DATA_PATH>/.api_secret`. Он
не логируется и не помещается в URL. Windows: файл должен иметь защищённый
owner-only DACL (владелец + SYSTEM); POSIX: режим `0600`. Отсутствующий,
некорректный или небезопасный секрет даёт fail-closed для PRIVATE/ADMIN; нода
не заменяет его молча.

Заголовки (принимается любой):

```
Authorization: Bearer <secret>
X-API-Secret: <secret>
```

`Host` запроса должен быть loopback (`127.0.0.1`, `localhost`, `::1`); `Origin`,
если присутствует, должен быть loopback или `null`. Привязка API к
`EServerType::LOCAL` сама по себе не является аутентификацией.

| Уровень | Для чего | Отказ |
|---|---|---|
| PUBLIC | read-only данные цепи/пиров, health | секрет не нужен |
| PRIVATE | status/peers/config на чтение | JSON-RPC `-32001`, REST `401` |
| ADMIN | запись config, ban/unban пиров, resync, shutdown | JSON-RPC `-32001`, REST `401` |

ADMIN: `ban_peer`/`unban_peer`, `update_config`, `/v1/resync`, REST ban/unban,
`/v1/shutdown`. PRIVATE: `get_status`, `get_peers`, `get_connected_peers`,
`get_config`. Foreign read-методы — PUBLIC.

## REST `/v1`

`GET /v1/` возвращает JSON-список маршрутов. Часть read-обработчиков не
проверяет HTTP-метод; используйте документированный метод и не строите
клиентов на неопределённом поведении.

| Метод и путь | Назначение | Статус | Примечания / ошибки |
|---|---|---|---|
| `GET /v1/health` | Готовность API/процесса | **IMPLEMENTED** | `{"status":"starting"\|"ready"}`. Доступен до открытия цепи, первый 200 обычно через `0,56–0,62 с`. `ready` — обработчики зарегистрированы, не синхронизация. Не GET → 404. |
| `GET /v1/status` | Состояние ноды/цепи/синхронизации | **PARTIAL** | Legacy-поля плюс Rust-совместимый объект `rust`; 500 без tip. Может быть 404 в первые миллисекунды и однажды превысил 5 с после проверки состояния; причина не установлена. |
| `GET\|POST /v1/resync` | Повторная синхронизация и unban всех | **IMPLEMENTED** | ADMIN. Побочные эффекты: `ResyncChain` + `UnbanAllPeers`; 401 без секрета. |
| `GET /v1/headers/<hash\|height\|commit>` | Один заголовок | **IMPLEMENTED** | 64-hex = hash, 66-hex = commitment, иначе десятичная высота. 400 `HEADER NOT FOUND`. |
| `GET /v1/blocks/<hash\|height\|commit>[?compact]` | Полный или компактный блок | **IMPLEMENTED** | 400 `BLOCK NOT FOUND`; `?compact` — компактный блок. |
| `GET /v1/chain` | Подтверждённый tip | **IMPLEMENTED** | `{height,last_block_pushed,prev_block_to_last,total_difficulty}`; 500 без tip. |
| `GET /v1/chain/outputs/byids?id=<commit>[,<commit>]…` | Позиции выходов по commitment | **IMPLEMENTED** | Массив `{commit,height,mmr_index}`; неизвестные id опускаются. 400 при ошибке разбора. |
| `GET /v1/chain/outputs/byheight?start_height=N&end_height=M` | Блоки с выходами в диапазоне высот | **IMPLEMENTED** | `end_height` по умолчанию `start_height`, если отсутствует или меньше start. 400 при неверных числах. |
| `GET /v1/peers/all` | Известные пиры | **IMPLEMENTED** | Массив объектов пиров. |
| `GET /v1/peers/connected` | Подключённые пиры | **IMPLEMENTED** | Массив объектов подключённых пиров. |
| `GET /v1/peers/diagnostics` | Агрегированная диагностика пиров/загрузки | **IMPLEMENTED** | `totals`, `banned`, `connected`, `download`, `versions`. Адреса — операционные данные; при публикации считать чувствительными. |
| `GET /v1/peers/<ip>` | Один известный пир | **IMPLEMENTED** | На новых кандидатах — **объект** (старый контракт — массив из одного). 404 `Peer not found.` |
| `POST /v1/peers/<ip>/ban` | Ручной ban | **IMPLEMENTED** | ADMIN; пустой успешный ответ. Порт парсится, но идентичность пира — только IP. |
| `POST /v1/peers/<ip>/unban` | Ручной unban | **IMPLEMENTED** | ADMIN; включает пиров только в БД. |
| `GET /v1/txhashset/roots` | Корни output/rangeproof/kernel на tip | **IMPLEMENTED** | 404 без tip. |
| `GET /v1/txhashset/lastkernels?n=N` | Последние N хешей ядер | **IMPLEMENTED** | Массив `{"hash":"…"}`; default 10, максимум 1000. Старые сборки возвращали строки. 400, если query не начинается с `n=`; 500 без TxHashSet. |
| `GET /v1/txhashset/lastoutputs?n=N` | Последние N хешей выходов | **IMPLEMENTED** | Массив `{hash}`; те же лимиты. |
| `GET /v1/txhashset/lastrangeproofs?n=N` | Последние N хешей rangeproof | **IMPLEMENTED** | Массив `{hash}`; те же лимиты. |
| `GET /v1/txhashset/outputs?start_index=N&max=M` | Страница unspent-выходов | **IMPLEMENTED** | `{highest_index,last_retrieved_index,outputs[]}`; `max` ограничен 1000. |
| `POST /v1/shutdown` | Штатное завершение | **IMPLEMENTED** | ADMIN; не POST → 404. Пустое тело при успехе. |

### Пример `/v1/status` (сокращённо)

```json
{
  "protocol_version": 1000,
  "user_agent": "Grin++ 1.2.9",
  "sync_status": "FULLY_SYNCED",
  "waiting_reason": "",
  "db_repaired": 0,
  "db_last_repair": 0,
  "block_sync": {
    "requests": 0, "timeouts": 0, "reassignments": 0,
    "cooldown_reassignments": 0, "retries": 0,
    "bans_fraud_height": 0, "bans_invalid_data": 0
  },
  "state": {"downloaded": 0, "download_size": 0, "processing_status": 100, "sync_failed": false},
  "network": {"height": 3324082, "total_difficulty": 0, "num_inbound": 0, "num_outbound": 2},
  "chain": {"height": 3324082, "hash": "…", "previous_hash": "…", "total_difficulty": 0},
  "header_height": 3324082,
  "rust": {
    "chain": "floonet",
    "protocol_version": 1000,
    "user_agent": "Grin++ 1.2.9",
    "connections": 2,
    "tip": {"height": 3324082, "last_block_pushed": "…", "prev_block_to_last": "…", "total_difficulty": 0},
    "sync_status": "no_sync"
  },
  "tip": {"height": 3324082, "last_block_pushed": "…", "prev_block_to_last": "…", "total_difficulty": 0},
  "connections": 2,
  "chain_type": "floonet",
  "sync_status_rust": "no_sync"
}
```

`pibd` (`received_segments`, `required_segments`) появляется только после старта
PIBD. `sync_info` появляется только при наличии реальных данных фазы.

## Foreign JSON-RPC (`POST /v2/foreign`)

Зарегистрированы все 13 имён Rust Grin 5.5.2. Регистрация не равна паритету
схем; проверка параметров/результатов/ошибок по каждому методу — **PARTIAL**,
если не указано иное.

| Метод | Параметры | Результат | Статус |
|---|---|---|---|
| `get_header` | `[height, hash, commit]` — минимум 3, nullable (лишние игнорируются) | `{"Ok": <header>}` | **IMPLEMENTED / PARTIAL** (схема не сверена построчно) |
| `get_block` | `[height, hash, commit]` — минимум 3, nullable (лишние игнорируются) | `{"Ok": <full block>}` | **IMPLEMENTED / PARTIAL** |
| `get_blocks` | `[start_height, end_height?, max?, include_proof?]` | `{"Ok": {"last_retrieved_height": N, "blocks": […]}}` | **IMPLEMENTED**; `max` ≤ 1000, диапазон ≤ 10000 высот. Ошибки `INVALID_PARAMS`, `QUERY_TOO_WIDE`, `NODE_BUSY_RETRY`, `INTERNAL_ERROR`. |
| `get_version` | нет | `{"Ok": {"node_version": "Grin++ 1.2.9", "block_header_version": N}}` | **IMPLEMENTED** |
| `get_tip` | нет | `{"Ok": {height,last_block_pushed,prev_block_to_last,total_difficulty}}` | **IMPLEMENTED** |
| `get_kernel` | `[excess, min_height?, max_height?]` | `{"Ok": {height, mmr_index, tx_kernel:{features,fee_shift,fee,lock_height,excess,excess_sig}}}` | **IMPLEMENTED**; использует persistent KernelIndex при совпадении с tip, иначе bounded direct scan. Ошибки `NOT_FOUND`, `INTERNAL_ERROR`. |
| `get_outputs` | `[[commits], start_height?, end_height?, include_proof?, include_merkle_proof?]` | `{"Ok": [<output>…]}` | **IMPLEMENTED**; ≤ 128 commitments и ≤ 100 высот за вызов. `merkle_proof` всегда `null` в этом срезе. |
| `get_unspent_outputs` | `[start_index, end_index?, max?, include_proof?]` | `{"Ok": {"last_retrieved_index": N, "highest_index": M, "outputs": […]}}` | **IMPLEMENTED**; `start_index` 1-based, `end_index` — 0-based исключающая граница PMMR, ≤ 8192 позиций и ≤ 10000 выходов. |
| `get_pmmr_indices` | `[start_block_height, end_block_height?]` | `{"Ok": {"last_retrieved_index": N, "highest_index": M, "outputs": []}}` | **IMPLEMENTED**; отображение диапазона высот в индексы output-MMR. |
| `get_pool_size` | нет | `{"Ok": n}` | **IMPLEMENTED** |
| `get_stempool_size` | нет | `{"Ok": n}` | **IMPLEMENTED** |
| `get_unconfirmed_transactions` | нет | `{"Ok": [{"src": "Broadcast"\|"Stempool", "tx": {…}}]}` | **IMPLEMENTED** |
| `push_transaction` | `[tx, fluff?]` | `{"Ok": null}` | **IMPLEMENTED / PARTIAL**; ошибки валидации/пула не полностью сопоставлены кодам Rust. |
| `get_config` | нет | объект config (PRIVATE) | **C++ extension**; отсутствует в foreign API Rust 5.5.2 |

При отсутствии/неверных параметрах — `PARAMS_MISSING`/`INVALID_PARAMS`;
неизвестный метод — `METHOD_NOT_FOUND` (`-32601`).

Пример:

```json
{"jsonrpc":"2.0","method":"get_tip","params":[],"id":1}
→ {"jsonrpc":"2.0","result":{"Ok":{"height":3324082,"last_block_pushed":"…","prev_block_to_last":"…","total_difficulty":0}},"id":1}
```

## Owner JSON-RPC (`POST /v2/owner`)

| Метод | Параметры | Результат | Доступ | Статус |
|---|---|---|---|---|
| `get_status` | нет | `{"Ok": <Rust Status>}` | PRIVATE | **IMPLEMENTED**; тот же builder, что `/v1/status.rust` |
| `get_peers` | `[address?]` | `{"Ok": [<PeerData>…]}` | PRIVATE | **IMPLEMENTED**; порт в адресе игнорируется (идентичность — IP) |
| `get_connected_peers` | нет | `{"Ok": [<PeerData>…]}` | PRIVATE | **IMPLEMENTED**; только поля подключённых |
| `ban_peer` | `[address]` | `{"Ok": null}` | ADMIN | **IMPLEMENTED** |
| `unban_peer` | `[address]` | `{"Ok": null}` | ADMIN | **IMPLEMENTED**; также удаляет пиров только в БД |
| `get_config` | нет | объект config | PRIVATE | **C++ extension** |
| `update_config` | объект config | `{"restart_required": false}` | ADMIN | **C++ extension**; validate → persist → apply; при ошибке runtime config не меняется |
| `validate_chain` | — | — | ADMIN | **NOT IMPLEMENTED** — не зарегистрирован; `METHOD_NOT_FOUND`. Backend-контракта нет. |
| `compact_chain` | — | — | ADMIN | **NOT IMPLEMENTED** — не зарегистрирован. |
| `reset_chain_head` | — | — | ADMIN | **NOT IMPLEMENTED** — не зарегистрирован. |
| `invalidate_header` | — | — | ADMIN | **NOT IMPLEMENTED** — не зарегистрирован. |

`update_config` принимает `min_peers`, `max_peers`, `min_confirmations`,
`reuse_address` (`0`/`1`), `preferred_peers`, `allowed_peers`, `blocked_peers`.
Списки пиров ≤ 256 записей. `min_peers` ≥ `min_sync_peers` и ≤ `max_peers`.

## Схема status и известные отличия от Rust

`/v1/status` сохраняет legacy-поля Grin++ и добавляет Rust-совместимый объект
`rust` плюс алиасы `tip`, `connections`, `chain_type`, `sync_status_rust`. Один
builder (`NodeStatus::Build`) питает и `/v1/status.rust`, и owner `get_status`,
поэтому они не расходятся.

Известные отличия от Rust 5.5.2 (**PARTIAL/OPEN**, намеренно не скрыты):

1. **Фаза PIBD.** Во время `SYNCING_TXHASHSET` C++ отдаёт
   `sync_status: "txhashset_download"` с legacy ZIP byte-счётчиками
   (`state.downloaded`, `state.download_size`). Rust отдаёт
   `txhashsetpibd_download` с `aborted`, `errored`, `completed_leaves`,
   `leaves_required`, `completed_to_height`, `required_height`.
   `pibd.received_segments`/`required_segments` — счётчики сегментов, не листья;
   переименовывать их в поля Rust нельзя.
2. **Фаза валидации.** Во время `PROCESSING_TXHASHSET` у C++ один грубый
   `processing_status`, отображаемый в Rust-имя
   `txhashset_rangeproofs_validation`. Rust разделяет setup, rangeproof
   (`rproofs`, `rproofs_total`), kernels (`kernels`, `kernels_total`), save и
   done. Точную Rust-схему для этих стадий не заявлять.
3. **Идентичность пира.** C++ ключует пиров по IP; порт парсится, но не входит в
   идентичность. Rust owner принимает `SocketAddr`. Для двух пиров на одном IP с
   разными портами поведение различается.
4. **Изменения REST-схем.** `GET /v1/peers/<ip>` теперь объект, а
   `lastkernels`/`lastoutputs`/`lastrangeproofs` — объекты `{hash: …}`. Внешние
   потребители после этого намеренного изменения полностью не перепроверены.
5. **Owner chain control.** `validate_chain`, `compact_chain`, `reset_chain_head`,
   `invalidate_header` отсутствуют. Фиктивного успеха нет.
6. **Расширения.** `get_config` (foreign и owner) и `update_config` (owner) —
   C++-расширения, не паритет Rust 5.5.2.

## Доступность и фазы старта

- `/v1/health` регистрируется до открытия цепи и отвечает `starting`, затем
  `ready` после создания REST/RPC-обработчиков. На профиле `ready` предшествовал
  `fully_synced` примерно на 19–25 с.
- `/v1/status` регистрируется вместе с полным REST-сервером, поэтому 404 в первые
  моменты ожидаем.
- Одно наблюдение на раннем бинарнике: `/v1/status` превысил 5 с после проверки
  состояния, пока `/v1/health` был доступен. Причина не установлена; задержку
  status под нагрузкой считать **PARTIAL/NOT VERIFIED**.

## Статус проверки этого документа

- Инвентаризация обработчиков и уровней доступа перепроверена по коду кандидата.
- Матрица из 27 локальных HTTP/RPC проверок (согласованность status, пустые
  массивы, неверные числа, owner auth, resync, shutdown, IPv6 ban/unban) прошла
  на промежуточных кандидатах (`C996B748…`, `36AD1C92…`, `D748B90E…`). На
  финальном `4039086C…` она в этом снимке **не перезапускалась**; паритет по
  каждому методу с Rust — **PARTIAL/OPEN**.
- Поведение API на Mainnet — **NOT VERIFIED**.

---

[English version](API.md) · [Deutsche Fassung](API.de.md)
