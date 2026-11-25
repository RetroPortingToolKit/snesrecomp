"""Returns that discard callers must not supply ordinary callee exit widths."""
from _helpers import make_lorom_bank0
from v2.decoder import decode_function, analyze_function_exit_mx_modes


def exits(code, m=1, x=0):
    graph = decode_function(make_lorom_bank0({0x8000: bytes(code)}),
                            0, 0x8000, m, x)
    return analyze_function_exit_mx_modes(graph)


def test_outer_caller_cleanup_does_not_contaminate_ordinary_exit():
    # Ordinary RTS; or discard two JSR frames and restore an outer PHB/PHP.
    # The outer return's transient M0 is not the callee's ordinary M1 exit.
    assert exits([0xD0, 0x01, 0x60, 0xFA, 0xFA, 0xC2, 0x20,
                  0xAB, 0x28, 0x60]) == {(1, 0)}


def test_pure_outer_caller_return_has_no_ordinary_exit():
    assert exits([0xFA, 0xFA, 0xAB, 0x28, 0x60]) == set()


def test_shared_return_site_preserves_its_normal_path():
    assert exits([0xD0, 0x01, 0xFA, 0x60]) == {(1, 0)}


def test_balanced_status_restore_remains_an_ordinary_exit():
    assert exits([0x08, 0xC2, 0x20, 0x28, 0x60]) == {(1, 0)}


def test_unknown_stack_height_remains_unproven():
    assert exits([0x9A, 0x60]) is None


def test_internal_stack_dispatch_remains_unproven():
    assert exits([0xF4, 0xFF, 0x8F, 0x60]) is None


def test_pha_tail_dispatch_consumes_synthetic_frame():
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30, 0xB9, 0x00, 0x90, 0x3A, 0x48,
                       0xE2, 0x30, 0x60]),
        0x9000: bytes([0x60]),
    })
    graph = decode_function(rom, 0, 0x8000, 1, 1, indirect_dispatch={
        0x008006: {'count': 1, 'idx_reg': 'Y', 'targets': [0x009000]},
    })
    assert analyze_function_exit_mx_modes(graph) == {(1, 1)}


def test_literal_pha_is_still_an_unproven_return_frame():
    assert exits([0xC2, 0x30, 0x48, 0xE2, 0x30, 0x60], 1, 1) is None
