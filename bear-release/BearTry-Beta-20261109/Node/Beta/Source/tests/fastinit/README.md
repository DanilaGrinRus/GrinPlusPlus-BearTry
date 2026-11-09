# FastInit acceptance harness

These scripts are test-only. They never use a live profile, never touch
`RC1`/`RC2`, and never bind `15013`/`15014`.

## Modes

`GRINPP_HEADER_CKPT` has exactly these values:

- `off` (or unset)
- `full`
- `hash`
- `checkpoint`  (internally `EMode::Write`)
- `fast`

`write` is not accepted and disables FastInit.

`GRINPP_HEADER_CKPT_BOOTSTRAP=1` is only meaningful together with
`GRINPP_HEADER_CKPT_TRUST_ANCHOR=<published aggregate>`. Bootstrap without an
anchor must refuse to create a checkpoint. An existing trusted `header.ckpt`
does not need bootstrap or an anchor.

## Core harness

```powershell
Copy-Item <profile> <work>\case -Recurse
.\Run-FastInitScenarios.ps1 `
  -Exe <merged>\GrinNode.exe `
  -SourceProfile <work>\case `
  -WorkRoot <work>\fastinit-core `
  -Case Core
```

The harness creates a fresh copy under `-WorkRoot`, uses ports 28913/28914,
and checks:

1. `hash` produces an aggregate.
2. `checkpoint` + `BOOTSTRAP=1` without anchor logs
   `header_checkpoint_not_written reason=trust_anchor_required` and creates
   no `header.ckpt`.
3. A wrong anchor logs `reason=trust_anchor_mismatch` and creates no file.
4. The correct anchor writes `trust=1`.
5. A `fast` restart logs `event=header_checkpoint_fast` without a preceding
   full fallback.

## Lock harness

```powershell
.\Run-FastInitScenarios.ps1 -Exe <merged>\GrinNode.exe `
  -SourceProfile <work>\case -WorkRoot <work>\fastinit-lock -Case Lock
```

The first instance uses ports 28913/28914. The second uses the same
`DATA_PATH` but ports 29013/29014. It must exit quickly with the explicit
`.fastinit-profile.lock` held by the first instance; exactly one GrinNode must
remain.

## Reorg harness

Reorg requires two prepared AUTOMATED_TESTING branches with a common genesis.
Use `Run-FastInitReorg.ps1` with copies only:

```powershell
.\Run-FastInitReorg.ps1 `
  -Exe <merged>\GrinNode.exe `
  -BranchA <copy-of-longer-branch> `
  -BranchB <copy-of-shorter-branch> `
  -WorkRoot <work>\fastinit-reorg
```

Expected: B's candidate tip becomes A's tip (real P2P fork/reorg), the old
checkpoint remains untouched, and a subsequent `fast` start does full
fallback and writes the new trusted checkpoint.

## Anchors

The correct anchor is the 64-hex aggregate for the **exact** current state,
published out-of-band with provenance and SHA-256. Do not use a checkpoint
from a different tip/count/MMR state. Self-computed aggregate is lab-only and
is not proof of trust.
