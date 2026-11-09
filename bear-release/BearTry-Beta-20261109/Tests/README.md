# Tests

- `Tests.exe` — Beta test binary
  (`2B5BD9853979ED7AEFB4B2C3E6D9B27B6BB397E6E816CE2D52FC9E2E74B25DF3`).
- `TESTS-SMOKE.log` — smoke-run artifact.
- Исторический full suite Beta-пары: 468 test cases / 19 303 assertions PASS.
- Свежий повторный прогон на 2026-11-09 падает в известный ASIO teardown
  (`0xC0000409`) до summary; это flaky harness, см. `..\docs\UNVERIFIED_AND_REMOVED.md`.

Запуск изолированно (пример):

```powershell
$env:HOME='C:\DHW\Gplus\Tests\home'
$env:USERPROFILE=$env:HOME
$env:TMP="$env:HOME\temp"; $env:TEMP=$env:TMP
.\Tests.exe --reporter console
```
