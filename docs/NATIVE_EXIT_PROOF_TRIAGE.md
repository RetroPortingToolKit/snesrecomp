# Native exit proof follow-up (#110)

The report compares Metal Marines at `bb37c87`: native analysis retains
interpreter fallbacks for variants that its Python counterpart proved. The
reported gameplay comparisons matched, so the evidence describes lost AOT
coverage, not demonstrated incorrect gameplay. Main now uses native analysis
only; removing the Python backend does not establish that the proof gap is fixed.

`tests/v2/test_native_php_plp_exits.py` exercises leaf and non-leaf PHP/PLP
wrappers, saved A/X/Y, and a shared tail in all four entry widths. These are
synthetic reductions, not the original 17-variant reproducer.

To complete the investigation, retain the ROM locally and supply the owning
cfgs, engine/native-binary commit identities, and the manifests for the same
roots. For each reported variant, compare its `reasons`, call demands and
`exit_modes`. Reduce the first missing proof to redistributable synthetic bytes
with the same control-flow/stack shape before changing the solver. Do not infer
that a callee preserves width merely because the proof is missing.

The private repository named by the report returned HTTP 404 to this review's
authenticated GitHub client. Consequently this draft does not assert that the
original gap is fixed or that the reported 825/809 AOT counts still reproduce.
