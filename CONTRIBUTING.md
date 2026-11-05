# Contributing

This is a community fork focused on the **node module first**. Contributions are
welcome when they keep the node honest, testable and safe. Wallet/UI work is a
separate track and is not part of Node Beta acceptance.

## Before you start

- Read [docs/STATUS.md](docs/STATUS.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
  and [KNOWN_ISSUES.md](KNOWN_ISSUES.md). Do not work from a historical report
  without checking the current tag.
- The canonical language of the code and documentation is English. A change
  that affects docs must update the English file; Russian/German translations
  follow. If a translation conflicts with English, English wins.
- Claims require evidence. "It builds" is not a test result; a mock is not a live
  run; a skipped fixture is not a PASS.

## Safety rules

- **No real funds.** Development and testing use Floonet/test chains only. Never
  ask a contributor to test against Mainnet with real money.
- **No secrets.** Never commit or paste seed words, private keys, `*.api_secret`,
  passwords, session tokens or personal profile data. If you find a secret in a
  tracked file, report it privately per [SECURITY.md](SECURITY.md).
- **Isolate profiles.** Use a dedicated `DATA_PATH` and separate ports. Do not
  share a profile between two instances.
- Do not copy user profiles, wallets, logs or evidence dumps into the repository.

## Development workflow

1. Fork and branch from `main`. Keep one logical change per pull request.
2. Use C++17 and the existing style. The build is `/W4 /WX`; new warnings are
   errors. Do not weaken warning levels, the static CRT or integrity checks to
   make a change compile.
3. Add tests for new behaviour and regressions. A defect fix should include a
   test that fails before the fix and passes after it when practical.
4. Run the relevant tests locally (see [docs/BUILD.md](docs/BUILD.md)):

   ```powershell
   .\bin\Release\Release\Tests.exe "~[.]"
   ```

   Then run the explicit tags that cover your change (`[pibd]`, `[fault]`,
   `[orphan]`, `BlockPipe*`, `PibdScheduler*`, `REORG*`, …).
5. Update documentation and evidence in the same pull request:
   - behaviour/API change → `docs/API.md` (+ ru/de);
   - status change → `docs/STATUS.md` (+ ru/de) and `docs/evidence/INDEX.md`;
   - limitation → `KNOWN_ISSUES.md`;
   - user-visible change → `CHANGELOG.md`.
6. In the pull request description state: what problem, what changed, what was
   verified (commands and results), what was **not** verified, and the residual
   risk. Bind results to the commit and, for binaries, to their SHA-256.

## Commit messages

The post-v1.2.9 history is **reconstructed**; see
[docs/HISTORY-RECONSTRUCTION.md](docs/HISTORY-RECONSTRUCTION.md). For new
commits:

- Use a clear subject in the imperative mood.
- Explain why in the body, not only what.
- Reference the evidence (report path or test command) that supports the change.
- Do not invent authors, dates or co-authors. Do not sign off on behalf of
  someone else.

## Licensing

The project is MIT. By contributing you agree that your contribution is licensed
under the same MIT terms. Preserve upstream copyright and the dependency notices
in `THIRD_PARTY_NOTICES.md`; if you add or update a dependency, update that file
and the pinned revision list in `docs/BUILD.md`.

Thirty files derive from the Rust `grin` 5.5.2 node (Apache-2.0, Copyright The
Grin Developers) and carry comment-only attribution headers; the canonical list
is `LICENSES/GRIN-DERIVED-FILES.md`. If you copy or adapt more Grin code, add the
same header and update that list and `NOTICE`; the Apache-2.0 §4(c) obligation is
already met for the listed files and must stay met. `Logo.png` is a PARTIAL
upstream asset (no separate upstream licence); do not re-license it or claim a
third-party licence for it without evidence.

`Tests.exe` is verification-only and is not part of the user-facing release
package; see [docs/BUILD.md](docs/BUILD.md) for the required isolation.

## Pull request checklist

- [ ] Builds Release x64 with `/W4 /WX` (Windows 11 x64, MSVC 14.44) or clearly
      states that only a static/unit check was possible.
- [ ] Relevant tests pass; skipped/fixture-dependent checks are named as skipped.
- [ ] No secrets, profiles, logs or absolute developer paths.
- [ ] Docs updated in English (canonical) and translations where applicable.
- [ ] Status labels are evidence-based: PASS / PARTIAL / FAIL / NOT VERIFIED /
      PLANNED / EXPERIMENTAL.
- [ ] No Mainnet/real-funds claim.
- [ ] License/notices updated if dependencies changed.

## Review and acceptance

A maintainer reviews the diff and the evidence. A change that alters consensus,
storage format, trust boundaries or release status needs an explicit design note
(ADR-style: context, decision, alternatives, consequences, verification).

## Code of conduct

Be technical, specific and civil. Critique evidence and code, not people. Do not
publish another person's logs, keys, addresses or profile data.
