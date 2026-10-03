"""A dispatch case must name the PC whose body actually exists.

Regression for the undefined cross-bank symbols Super Metroid players hit
through the launcher's "Generate & rebuild" wizard (11 of them under
--cfg-roots; `MotherBrain_CalcHdma_*_M0X0`, `bank_88_B279_M1X1`, ...).

LoROM maps $80-$BF onto the same ROM bytes as $00-$3F, so when a target has
no AOT body of its own, `valid_variant_list` answers through its mirror. The
bodies there are real -- but they were emitted under the MIRROR's symbol
name, and the two sides of a mirror pair routinely carry different names
because `_cfg_name_maps` claims friendly names globally first-come-first-
served. Emitting the survivor set from one PC and the symbol from the other
produces a call to a function nothing defines: the generate succeeds, and
the link fails minutes later.

Measured on Super Metroid: $88:B279 has no emitted variant, mirror $08:B279
has M1X1, and the switch emitted `bank_88_B279_M1X1(cpu)` for a body that
exists as `FxTypeFunc_2_Lava_M1X1`.
"""
from v2.codegen import (  # noqa: E402
    resolve_variant_owner, set_emitted_names, set_name_resolver,
    set_valid_variants, valid_variant_list, variant_dispatch_case_lines)


LLE = "interp_tier_run_call_frame(cpu, 0x88b279u, 0x89ac51u, 3, NULL)"


def _install(valid, names):
    set_valid_variants(valid, authoritative=True)
    # Both maps, as program_emit installs them: the resolver aliases across
    # the LoROM mirror, the emitted-name map does not.
    set_name_resolver(names)
    set_emitted_names(names)


def _reset():
    set_valid_variants({})
    set_name_resolver({})
    set_emitted_names({})


def test_owner_is_the_target_when_it_has_its_own_bodies():
    _install({0x88B279: frozenset({(1, 1)}), 0x08B279: frozenset({(0, 0)})},
             {0x88B279: "OwnName", 0x08B279: "MirrorName"})
    try:
        assert resolve_variant_owner(0x88B279) == 0x88B279
        assert valid_variant_list(0x88B279) == ((1, 1),)
    finally:
        _reset()


def test_direct_call_does_not_claim_a_body_only_emitted_at_the_mirror():
    from v2.codegen import has_exact_variant
    _install({0x118A49: frozenset({(0, 0)})},
             {0x118A49: "DeclaredBody", 0x918A49: "bank_91_8A49"})
    try:
        assert has_exact_variant(0x118A49, 0, 0)
        assert not has_exact_variant(0x918A49, 0, 0)
        # Switch-based dispatch has its own owner/name resolution contract.
        assert resolve_variant_owner(0x918A49) == 0x118A49
    finally:
        _reset()


def test_owner_falls_through_to_the_mirror_when_the_target_has_none():
    _install({0x08B279: frozenset({(1, 1)})},
             {0x08B279: "FxTypeFunc_2_Lava"})
    try:
        assert resolve_variant_owner(0x88B279) == 0x08B279
        assert valid_variant_list(0x88B279) == ((1, 1),)
    finally:
        _reset()


def test_dispatch_case_names_the_mirror_body_not_the_caller_s_bank():
    """The bug: survivor set from $08:B279, symbol from $88:B279."""
    _install({0x08B279: frozenset({(1, 1)})},
             {0x08B279: "FxTypeFunc_2_Lava"})
    try:
        lines = variant_dispatch_case_lines(
            0x88B279, "bank_88_B279", lle_fallback=LLE)
        body = "\n".join(lines)
        assert "FxTypeFunc_2_Lava_M1X1(cpu)" in body, body
        assert "bank_88_B279_M1X1" not in body, (
            "named the caller's bank for a body that lives on the mirror")
        # The three widths with no body still tier to LLE, never to a
        # sibling: width identity stays exact.
        assert body.count(LLE) == 4  # cases 0,1,2 + default
    finally:
        _reset()


def test_mirror_body_with_no_friendly_name_uses_the_synthetic_mirror_name():
    """The other half of SM's failure: the mirror fell back to synthetic.

    $2D:E293 took the cfg label `MotherBrain_CalcHdma_Down_Down`, so its
    mirror $AD:E293 -- which is where the only body was emitted -- got
    `bank_AD_E293`.
    """
    _install({0xADE293: frozenset({(0, 0)})},
             {0x2DE293: "MotherBrain_CalcHdma_Down_Down"})
    try:
        lines = variant_dispatch_case_lines(
            0x2DE293, "MotherBrain_CalcHdma_Down_Down", lle_fallback=LLE)
        body = "\n".join(lines)
        assert "bank_AD_E293_M0X0(cpu)" in body, body
        assert "MotherBrain_CalcHdma_Down_Down_M0X0" not in body, body
    finally:
        _reset()


def test_no_bodies_on_either_side_stays_on_the_caller_s_pc():
    """Nothing to name after: the target keeps its own identity and the
    switch is all LLE. Falling to the mirror here would invent a symbol."""
    _install({}, {})
    set_valid_variants({0x999999: frozenset({(1, 1)})}, authoritative=True)
    try:
        assert resolve_variant_owner(0x88B279) == 0x88B279
        lines = variant_dispatch_case_lines(
            0x88B279, "bank_88_B279", lle_fallback=LLE)
        body = "\n".join(lines)
        assert "bank_88_B279_M" not in body, body
        assert body.count(LLE) == 5  # all four cases + default
    finally:
        _reset()


def test_high_bank_without_a_lorom_mirror_never_mirrors():
    """$C0-$FF are not LoROM-mirrored; resolution must not wrap into them."""
    _install({0x4A8000 ^ 0x800000: frozenset({(1, 1)})}, {})
    try:
        assert resolve_variant_owner(0x4A8000) == 0x4A8000
    finally:
        _reset()
