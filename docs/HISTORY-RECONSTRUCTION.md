# History reconstruction

This repository preserves the real upstream history of
[GrinPlusPlus](https://github.com/GrinPlusPlus/GrinPlusPlus) up to tag `v1.2.9`
(commit `a0958a391c30cf630e3e08534fbc6fe9fb194326`, 2024-10-22). Everything
after that tag is a **reconstruction**.

## Why the history is reconstructed

The 2026 fork development happened in a local workspace whose source tree began
from a **single squashed import commit**. Intermediate commits, branch names,
rebase points, abandoned experiments and their dates were not preserved. There
is no honest way to recover the original chronology from the final tree.

Instead of inventing a plausible history, the fork does the following:

1. Keeps upstream `v1.2.9` and all earlier commits untouched.
2. Takes the verified final tree (the accepted candidate) as the source of
   truth.
3. Splits the verified final diff into logical commit groups by purpose and
   dependency order.
4. Marks every reconstructed commit as reconstructed, with an honest
   fork-preparation identity and evidence references in the message trailer.
5. Publishes the final source manifest and per-group patches so that the result
   can be audited independently.

## What "reconstructed" means here

- The **content** of the final commit is verified: the tree must match the
  candidate source manifest (`964B2DD8…`, 1528 files, excluding documented
  omissions such as build outputs, profiles and local audit dumps). The
  verification report records: 1386 files checked, 1384 identical after
  CR-normalisation, 2 intentional differences (`.gitignore`,
  `OMITTED_LOCAL_TOOLS.md`), 0 content differences and 0 missing files —
  **PASS**.
- The tag `node-beta.1-candidate` is an annotated tag (object
  `317d143d2fc0d8b7ca8a7190521bca3e2f211a38`) whose tagged commit is
  `7efdb02e2a4d714d406b88b5610cb6e1c9d2bf59`.
- The **ordering** is logical, not chronological. A file touched by several
  workstreams is assigned to the earliest group that needs it; later groups
  build on it.
- **Authorship and dates** are honest: reconstructed commits use a
  fork-preparation identity and the actual commit time. They do not pretend to
  be written by upstream maintainers or by the original 2026 development
  session.
- Historical results from the development workspace (RC1/RC2, intermediate
  binaries) are **evidence**, not part of this git history. They are labelled by
  their own SHAs and never transferred to the candidate.

## How the grouping was chosen

The groups follow the dependency order of the node:

1. build/test infrastructure and repository hygiene;
2. wire-format and error-handling hardening;
3. storage consistency and recovery;
4. PIBD models, messages and scheduler;
5. PIBD download, validation, activation and resume;
6. multi-peer PIBD and range-proof worker policy;
7. KernelIndex;
8. API, authentication and diagnostics;
9. FastInit header checkpoint (experimental, opt-in);
10. wallet fixes actually present in the final diff;
11. license, notices and tooling.

The mapping from group to changed paths, problem statement, tests and residual
risk is in
[`docs/history/FROM-UPSTREAM-TO-CURRENT.en.md`](history/FROM-UPSTREAM-TO-CURRENT.en.md).

## Post-tag commits

Publishing the fork added commits on top of the tagged candidate. They do not
change the candidate source tree verified against the manifest:

- `33d89265` and `b7223ede` — remove private absolute paths from audit/tooling
  text;
- `94a90aa1` — history reconstruction note (this file's first version);
- `2aea52aa` — replace the legacy upstream CI workflow with a Windows
  static-sysroot build; **prepared, not executed on GitHub Actions**;
- `15e231b9` — portable dependency sysroot helper (`tools/deps/build_sysroot.py`,
  pinned refs); reviewed and dry-run, **end-to-end rebuild NOT VERIFIED**;
- `8d5e9842` — public documentation (this documentation set).

Read `git log --oneline node-beta.1-candidate..main` for the exact current list.

## How to audit the reconstruction

1. Check out `node-beta.1-candidate`.
2. Compare the tree against `docs/evidence/files/candidate-source-manifest-final.sha256`
   (excluding documented omissions).
3. Read the reconstructed commits in dependency order; each message names its
   group and evidence.
4. Cross-check each claim in the history document against
   [`docs/evidence/INDEX.md`](evidence/INDEX.md) and the public evidence files.
5. Upstream files before `v1.2.9` should be byte-identical to upstream; the
   earlier tree comparison (1045 files, zero differences) is recorded in the
   preparation reports and is not re-invented here.

## What is intentionally excluded

- Build outputs (`bin/`, `build/`), profiles, node data, logs and databases.
- Local API secrets, seeds, private keys, tokens and personal data.
- Machine-specific audit evidence dumps and large binaries that are not part of
  the source.
- Local helper scripts that only worked on the original machine; the tooling
  that is published is the portable subset.

## Corrections and provenance

- The earlier `source-manifest.sha256` in the development workspace is **stale**
  (five files were edited and binaries rebuilt after it was generated). The
  authoritative manifest is the final one cited above.
- If a later change is needed, it is a new commit on top of this history; the
  reconstructed past is not rewritten to look better.
- This note is part of the evidence: a reader must be able to tell what is
  upstream history, what is reconstructed, and what is a claim without evidence.

---

See also: [../README.md](../README.md) ·
[docs/history/FROM-UPSTREAM-TO-CURRENT.en.md](history/FROM-UPSTREAM-TO-CURRENT.en.md) ·
[docs/evidence/INDEX.md](evidence/INDEX.md)
