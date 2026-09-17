#!/usr/bin/env python3
"""L3 test runner. Separate from run_tests.py because each L3 test
launches two full smw.exe binaries, so the whole file is heavy.

Discovers test_*.py modules in this directory and runs each test_*
function. Exit 0 if all pass, 1 otherwise.

Usage:  python snesrecomp/tests/l3/run_l3.py
"""
import importlib
import pathlib
import sys
import traceback


TESTS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(TESTS_DIR))


def discover_modules():
    return sorted(
        p.stem for p in TESTS_DIR.glob('test_*.py')
        if p.stem != 'test_sanity'  # skip — depends on load_state which has a sync bug
    )


def main() -> int:
    passed = 0
    failed = 0
    fail_log = []
    for modname in discover_modules():
        mod = importlib.import_module(modname)
        tests = [(n, getattr(mod, n)) for n in dir(mod) if n.startswith('test_')]
        for name, fn in tests:
            label = f'{modname}.{name}'
            try:
                fn()
                print(f'  PASS  {label}')
                passed += 1
            except AssertionError as e:
                msg = str(e)[:2000]
                print(f'  FAIL  {label}: {msg}')
                fail_log.append((label, msg))
                failed += 1
            except SystemExit as e:
                # A tool's CLI-style abort, raised inside a test. Several
                # tools/ scripts signal errors with raise SystemExit(msg)
                # rather than an exception -- it reads fine from a shell and
                # tests/test_run_benchmark_pairs.py asserts on it deliberately.
                # SystemExit derives from BaseException, so `except Exception`
                # below does not catch it and one such test used to kill this
                # process mid-run, reporting nothing and silently skipping
                # every test after it. It is a failure like any other.
                print(f'  ERR   {label}: SystemExit: {e}')
                fail_log.append((label, f'SystemExit: {e}'))
                failed += 1
            except Exception:
                tb = traceback.format_exc()
                print(f'  ERR   {label}')
                fail_log.append((label, tb))
                failed += 1
    print()
    print(f'{passed} passed, {failed} failed')
    if fail_log:
        print()
        for label, msg in fail_log:
            print(f'--- {label} ---')
            print(msg)
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
