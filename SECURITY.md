# Security policy

## Scope

This repository is a **development snapshot** of a Grin++ fork. It is not a
production wallet and not Mainnet-ready. Security reports are taken seriously,
but no bug bounty, SLA or "supported release" is promised.

In scope:

- the node (`GrinNode`): P2P message handling, sync/PIBD, storage, API/RPC,
  authentication, denial-of-service boundaries;
- build and release artefacts produced from this repository;
- documentation that could cause unsafe operation (for example a wrong default
  or a missing warning).

Out of scope for this release:

- the wallet and UI as products — they are not part of Node Beta acceptance;
- the upstream project's own code unless the issue is introduced or worsened by
  this fork;
- attacks that require a compromised host, physical access, or a malicious
  operating-system/administrator.

## Reporting a vulnerability

**Do not open a public issue for a security problem.** Use the repository's
private advisory channel (GitHub "Report a vulnerability" / private security
advisory) or contact the maintainers privately through the address published on
the repository profile. If those channels are unavailable, open a minimal public
issue that says only that you have a security report and asks for a private
channel — do not include details.

Please include:

- affected commit/tag and binary SHA-256;
- platform and network (Mainnet/Floonet/test);
- a minimal reproduction and the impact;
- whether the issue is already public;
- a safe contact method.

Never send seed words, private keys, API secrets or personal data. If a secret is
already exposed, rotate it first and tell us only that it was exposed.

## Safe testing rules

- Use **Floonet or a test chain**. Do not test against Mainnet or with real
  funds.
- Use an isolated `DATA_PATH`, separate ports and a throwaway profile.
- Do not attack other people's nodes, peers or infrastructure. Denial-of-service
  testing must stay inside your own isolated environment.
- Do not publish other users' IP addresses, profiles, logs or keys.
- Stop the node gracefully when you are done; do not leave test processes
  running on shared machines.

## Known security-relevant design facts

- The API is bound locally and PRIVATE/ADMIN methods require a 64-hex
  owner-only secret (`.api_secret`). Binding locally is not authentication by
  itself; the secret and loopback Host/Origin checks are.
- The secret file is created owner-only and fails closed if malformed or
  insecure. Fix the file ACL instead of weakening the check.
- Peer messages are untrusted input; malformed messages and wrong-root segments
  are rejected. A local processing error does not ban a peer.
- Admin operations (`update_config`, `resync`, peer ban/unban, `shutdown`)
  require the secret.
- FastInit is experimental and requires an explicit trust anchor; do not enable
  it on a profile you cannot re-verify.
- Wallet code in the tree is not accepted for this release. Do not use it with
  real funds.

## Disclosure

We aim to acknowledge a private report, reproduce it in an isolated environment,
fix the root cause and add a regression test before public disclosure. Timelines
depend on maintainer availability; this is a volunteer project. We will credit
reporters who ask to be credited and will not publish personal data.

## No warranty

The software is provided "as is", under the MIT license, without warranty of any
kind. See [LICENSE](LICENSE).
