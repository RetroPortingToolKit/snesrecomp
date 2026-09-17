# tools/

Scripts that run beside the recompiler. Most are internal to this repo and can
be moved, renamed or deleted freely. **Some cannot**, and the difference is not
visible from inside this repository, which is what this file is for.

## The published surface

Every path in the table below is invoked *by path* from another repository.
There is no indirection: a game's workflow runs
`python3 snesrecomp/tools/rom_identity.py`, and that either resolves or the
release fails. Python has no equivalent of the include-path trick that let
`runner/src` be reorganised without touching a single consumer
(`docs/RUNNER_LAYOUT.md`) — a moved script is simply gone.

**Do not move, rename or delete anything in this table without updating the
callers named beside it, in the same change.** `0a947f4` is the worked example
of skipping that step: 76 pure renames that updated no consumer, delivered to
three game repos as what looked like three unrelated bugs of their own.

| Path | Contract | Called by |
|---|---|---|
| `tools/rom_identity.py` | **hard** | 5 × `.github/workflows/release.yml` and 4 × `scripts/package_release.sh` (SecretOfEvermore, StarFox, SuperMetroidRecomp, SuperMetroidSNESRecomp, ZeldaAlttP); `retcomm-studio` project templates |
| `tools/v2_emit.py` | **hard** | `MegaManXSNESRecomp/tools/bootstrap.sh`; named in SuperMarioWorld, SuperMetroid, SuperSmashWorld docs |
| `tools/tier2_ingest.py` | **hard** | `GundamWingEndlessDuelSNESRecomp/tools/regen.sh` |
| `tools/ingest_sm_decomp.py` | **hard** | `SuperMetroidRecomp/tools/ingest_sm_decomp.py`, `SuperMetroidRecomp/refs/snesrev-sm.pin` |
| `tools/v2_regen.py` | soft | SuperMarioWorld, SuperMetroidRecomp, SuperSmashWorld — developer docs |
| `tools/v2_sync_funcs_h.py` | soft | SuperMarioWorld, SuperMetroidRecomp, SuperSmashWorld — developer docs |
| `tools/build_native_analyzer.py` | soft | MegaManXSNESRecomp — developer docs |
| `tools/check_link_closure.py` | soft | SuperMetroidRecomp — developer docs |
| `tools/generate_ci.sh` | soft | `retcomm-studio` new-project docs |

**hard** means a script, workflow or template executes it; breaking it breaks a
build or a release. **soft** means a game repo's prose tells a human to run it;
breaking it wastes that person's afternoon instead.

`tests/v2/test_published_tools.py` reads this table and fails if any path in it
is not there. That gate only sees the left column — it cannot tell whether the
right column is still true, so when you add a caller in a game repo, add the
row here too.

## Named by a game repo, but never present here

`SuperMarioWorldRecomp/CFG_AUDIT_SPIKE.md` and `SuperSmashWorld/CFG_AUDIT_SPIKE.md`
both name `snesrecomp/tools/cfg_override_validator.py` and
`snesrecomp/tools/cfg_override_triage.py`. Neither has ever existed in this
repo. They are a spike's proposal, not a missing file — recorded here so the
next person to grep for them stops at this line instead of assuming a
regression. The existing `cfg_*` scripts are the shipped cfg tooling.

## Everything else

Unlisted scripts are internal. The obvious prefix families — `cfg_*`,
`cosim_*`, `smwdisx_*`, `sdk_*`, `smoke_*`, `prepare_*`, `rb_*` — are
candidates for layer folders the way `runner/src` got them. The reason that
has not happened is in this file: four of the nine published paths sit inside
those families, so folding the families into folders would either split them
across two locations or move a path another repo executes.
