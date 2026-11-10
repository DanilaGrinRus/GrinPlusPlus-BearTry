# Source — recovered Beta source

## Что лежит

`Node/Beta/Source` — принятый recovered source:

- база: `source-vcpkg.tar.gz` (GCS)
  `FE1C2E53F00D94EB7DB661F0083F1B9147C9110BDA7EA316DA9669E97F161DB6`;
- точный task-72 patch:
  `C4E042F1C52D4BAFD62606D2EAF63912092354BF20F517FDFE5F94BA6ADA0AC1`;
- `WriteTorrcContent` в B565 не было; в текущем Source он появляется только как
  часть wallet TorConfig overlay.

## Provenance

`Node/Beta/Source/SOURCE_PROVENANCE.json`:

- status: recovered/buildable, **NOT byte-identical B565**;
- `binary_source_exact=false`;
- build verified: `GrinNode.exe` и `Tests.exe` собираются на локальном sysroot;
- SHA пересборки не совпадает с B565;
- `Source/RECOVERY_SOURCE_SHA256.txt` — SHA архивов/файлов.
- Frozen source tree: `B698079BBA24BFEDCD8F33BA4ACDDA669820ABAA1D6FC189C12B05B26D2A3609`
  (1548 files, `SOURCE_MANIFEST.sha256`).

## Сборка

```powershell
cmake -S Node\Beta\Source -B Node\Beta\build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=<workspace>\.toolchain\sysroot `
  -DGRINPP_TESTS=ON -DGRINPP_TOOLS=OFF
cmake --build Node\Beta\build --config Release --target GrinNode Tests `
  --parallel 1 -- /p:TrackFileAccess=false
```

## Состав

`src/`, `include/`, `tests/`, `cmake/`, `deps/`, `vcpkg/`, `tools/`,
`CMakeLists.txt`, `BUILD.md`.

## Важно

- Source не заменяет frozen binary; release — `B565612B…`.
- Точный per-file B565 manifest отсутствует; byte-equal SHA не гарантируется.
- Свежий full-suite PASS не заявляется (ASIO teardown); исторический PASS —
  в `BETA.md`.
