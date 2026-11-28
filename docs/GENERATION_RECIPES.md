# Project generation recipes

`generate --cfg-dir recomp` automatically reads `recomp/generate.toml`.
The setup launcher's Generate & Rebuild uses that same SDK entry point.
Game-specific source transformations stay in the game repository; the SDK
owns identity checking, analysis, hook execution, header synchronization and
publication. `tools/regen_project.sh --project-root <game> --no-tests` is the
portable developer wrapper. Generation does not run the engine suite unless
`--tests` is supplied. Add `--strict-idempotent` to compare two complete
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

For regional configurations, `project_root` is relative to the recipe's CFG
directory; `identity_file`, `out_dir`, `source_roots` and hook paths are relative
to that project root. The wrapper accepts `--cfg-dir`, `--out-dir`, and repeated
`--profile` paths. `--no-default-profiles` replaces recipe captures with the
explicit captures. `generation_cfg_dir` lets a prepare hook assemble a separate
CFG tree without overwriting the project's source overlays.

An `[emitter]` table can set `max_insns`, `max_nodes`,
`bank_shard_threshold_kib`, `bank_shard_pc_span` and `no_hle` (which controls
automatic emitted aliases, independently of the host-native runtime profile).
The numeric settings also have developer-wrapper flags.

`[[image_overlays]]` tables contain a project-relative IPS `file` and its exact
`sha256`. They require an original-image SHA-256 in the identity file. The SDK
checks the original image, patch fingerprint, record framing, bounds and
conflicting writes before prepare hooks. The compiler applies the overlay only
to loaded bytes and sends that view to the native analyzer over binary stdin;
the original image remains the `--rom` input and is never rewritten.
`.snesrecomp-image-view.json` records the original digest, loaded-image digest,
ordered overlay fingerprints and plan digest. The generated module still names
the loaded image it implements; generation does not enable a runtime mod.
