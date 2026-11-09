# Operations — запуск, VPS, HTTP

## Запуск Beta

```powershell
.\Node\Beta\start-node.ps1 -ConfigPath <config.json>
```

Launcher всегда: `--mainnet --headless --no-wallet`.
Wallet+node — отдельный трек.

## HTTP (read-only, на 2026-11-09)

Windows Beta VPS: `http://<vps>:8080`

- `/` — индекс
- `/status` — состояние node
- `/Node.log` — лог
- `/sync-stages.log` — стадии sync
- `/node.err` — stderr

Linux Docker builder: `http://<vps>:8080`

- `/docker-build.log`
- `/GrinNode` — Linux binary, если готов
- `/grinpp-linux-image.tar.gz` — image, если готов

IP/VM могут меняться; проверять GCP instance перед использованием.
Endpoint не входит в Beta freeze; секретов нет.

## VPS статус

- Windows VPS: node синкается (`SYNCING_HEADERS`, ~1.6M headers), 24h не
  завершены.
- Linux Docker/vcpkg build: отдельный трек; image/binary не входят в Beta.
- FreeBSD/macOS: вне трека/невозможны на GCP.

## Ограничения

- Public inbound/direct firewall — NOT VERIFIED.
- Relay ingress через GCP — PASS с ограничением (source виден как 127.0.0.1).
- 24h stability — NOT VERIFIED.
