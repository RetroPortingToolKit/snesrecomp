# Project generation recipes

`generate --cfg-dir recomp` automatically reads `recomp/generate.toml`.
The setup launcher's Generate & Rebuild uses that same SDK entry point.
Game-specific source transformations stay in the game repository; the SDK
owns identity checking, analysis, hook execution, header synchronization and
publication. `tools/regen_project.sh --project-root <game> --no-tests` is the
portable developer wrapper. Add `--strict-idempotent` to compare two complete
generated trees, including postprocessing.

```toml
schema = 1
identity_file = "rom_identity.txt"  # relative to the project root
cfg_roots = true
bind_legacy_profiles = true
historical_profiles = ["tier2_coverage.json"] # relative to cfg-dir

[[postprocess]]
script = "tools/apply_overrides.py" # relative to the project root
args = ["--gen-dir", "{out_dir}"]
```

`profiles` supplies current captures; `historical_profiles` retains older
analysis seeds without merging costs or identities across executable builds.
`bind_legacy_profiles` explicitly associates unbound v1 captures with the
configured, verified SHA-256. It refuses to bind when no expected SHA-256 is
configured. Observations still pass the normal eligibility, M/X, emulation,
return-contract and replay requirements; a recipe does not authorize AOT.

Optional `no_host_root_scan` preserves projects whose static policy excludes
host source discovery. `[[prepare]]` hooks run after ROM verification and before
analysis, for project-owned symbol import. `[[postprocess]]` hooks receive fresh
unpublished output, and failure preserves the live generated tree and funcs.h.
Prepare hooks may update project metadata; they should be idempotent. The
generated tree is published before the optional header replacement; these are
separate filesystem operations, not a crash-atomic transaction across paths.

Hooks execute with the SDK's Python interpreter and an argv array, never a
shell. Supported argument fields are `{rom}`, `{cfg_dir}`, `{out_dir}`, and
`{project_root}`. Output goes through SDK progress logging, preserving clean
JSONL for the launcher. The recipe and hook source hashes participate in the
generation identity; this alone does not establish netplay compatibility.

Identity values supplied by a launcher must agree with the identity file.
Recipe paths stay within the project, and missing profiles or scripts fail
before generation. Projects without a recipe retain the existing SDK behavior.
