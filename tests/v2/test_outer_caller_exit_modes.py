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
