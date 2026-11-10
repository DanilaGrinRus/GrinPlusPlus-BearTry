# Linux build — 2026-11-10

Built from the recovered Linux source snapshot, not from Windows B565 sources.

| File | Size | SHA-256 |
|---|---:|---|
| `GrinNode` | 14 871 936 | `078EF85D24B338CF6A18A058C9940B4BE4EABE55433C92C96C1EB2556AC5FC1A` |
| `Tests` | 19 746 328 | `729F42A62856A094CF1ECE3860F52F1442BF8DC0EE1DD5AF04B3182748924523` |
| `GrinNode-linux-image.tar.gz` | 44 501 870 | `49737E188D95FEDEDD080F29C50E4AE3425F8F5B92774696B6266F4669FB15A0` |

- Image ID: `6f6049ce8e7f…`
- Source archive: `source-linux-rpcfix-vcpkg.tar.gz`
  `6BEA97B08EB9BA9D7F01DE8838490431E0DD0BF7EDE192642B307FF0807745FC`
- Source files: 1544; tree `219C9D03952F83E4649EEF8986EC63BA2B8C9BF089E99E7423F02D3E685CA1A9`
- Explicit patches: `linux-rpc.patch`, `linux-acl.patch`, `linux-tests.patch`
  (in `Gplus/evidence/linux-build-20261110/`).
- Dockerfile SHA: `083EC80A…` (source build) / current runtime uses same source.

## Tests

- Full Linux suite: **PASS — 464/464 cases, 19 282 assertions**.
- API auth (`Owner RPC methods*`): **PASS — 8 assertions, 1 case**.
- P2P handshake (`HandShake*`): **PASS — 14 assertions, 2 cases**.
- `TorConfig*`: **NOT PRESENT in Linux source snapshot** (NOT VERIFIED).
- ACL repair is a Windows-only path; Linux uses POSIX file-mode checks.
  Linux `.api_secret` ACL repair: **NOT VERIFIED / not applicable**.
- Mainnet sync on Linux: **NOT VERIFIED**.

## Runtime

- `GrinNode --help`: PASS.
- `ldd`: all shared libraries resolved (`libminizip.so.1`, `libsodium.so.23`,
  `libuuid.so.1`, `libz.so.1`).
- Isolated profile smoke: `/v1/health=ready`, `/v1/status` `chain_type=main`,
  `SYNCING_HEADERS`.
