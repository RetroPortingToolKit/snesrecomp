"""Synchronize a scaffold's symbols.toml into owned blocks in bank configs.

The TOML contains names and tier choices, never instruction bytes. Keep manual
cfg directives outside the marked blocks. Explicit AOT roots are returned to
the emitter so emit=true does not depend on --cfg-roots or vector reachability.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import tempfile
import tomllib


BEGIN = "# >>> BEGIN symbols.toml (generated — do not edit)"
END = "# <<< END symbols.toml"
_BANK = re.compile(r"bank([0-9a-fA-F]{2})\.cfg")
_NAME = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")


@dataclass(frozen=True)
class FunctionSymbol:
    name: str
    bank: int
    addr: int
    emit: bool
    entry_m: int
    entry_x: int

    @property
    def pc24(self) -> int:
        return self.bank << 16 | self.addr


def _number(value, maximum, field, context):
    try:
        # Quoted addresses are hexadecimal, as in the scaffold's addr="8000".
        number = int(value, 16) if isinstance(value, str) else value
    except ValueError:
        number = None
    if type(number) is not int or not 0 <= number <= maximum:
        raise ValueError(f"{context}: {field} must be an integer in 0..{maximum:#x}")
    return number


def load_symbols(path: Path) -> list[FunctionSymbol]:
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    functions = data.get("func", [])
    if not isinstance(functions, list):
        raise ValueError(f"{path}: expected [[func]] tables")
    result = []
    names = {}
    addresses = {}
    for index, item in enumerate(functions, 1):
        context = f"{path}: func #{index}"
        if not isinstance(item, dict):
            raise ValueError(f"{context}: expected a table")
        name = item.get("name")
        if not isinstance(name, str) or not _NAME.fullmatch(name):
            raise ValueError(f"{context}: name must be a C identifier")
        context += f" ({name})"
        emit = item.get("emit", False)
        if type(emit) is not bool:
            raise ValueError(f"{context}: emit must be true or false")
        symbol = FunctionSymbol(
            name,
            _number(item.get("bank", 0), 0xFF, "bank", context),
            _number(item.get("addr"), 0xFFFF, "addr", context),
            emit,
            _number(item.get("entry_m", 1), 1, "entry_m", context),
            _number(item.get("entry_x", 1), 1, "entry_x", context),
        )
        previous = names.get(name)
        if previous is not None and previous != symbol.pc24:
            raise ValueError(f"{context}: name already used at ${previous:06X}")
        names[name] = symbol.pc24
        policy = (emit, symbol.entry_m, symbol.entry_x)
        previous = addresses.get(symbol.pc24)
        if previous is not None and previous != policy:
            raise ValueError(f"{context}: conflicting tier/entry modes at ${symbol.pc24:06X}")
        addresses[symbol.pc24] = policy
        if symbol not in result:
            result.append(symbol)
    return sorted(result, key=lambda s: (s.bank, s.addr, s.name))


def _split_block(text: str, path: Path):
    lines = text.splitlines(keepends=True)
    starts = [i for i, line in enumerate(lines)
              if line.startswith("# >>> BEGIN symbols.toml")]
    ends = [i for i, line in enumerate(lines)
            if line.rstrip() == END]
    if not starts and not ends:
        return text + ("\n" if text and not text.endswith("\n") else ""), "", False
    if len(starts) != 1 or len(ends) != 1 or starts[0] >= ends[0]:
        raise ValueError(f"{path}: malformed symbols.toml generated block")
    return "".join(lines[:starts[0]]), "".join(lines[ends[0] + 1:]), True


def sync_symbols(cfg_dir: Path) -> list[FunctionSymbol]:
    """Validate all inputs before changing any cfg; absent TOML is a no-op."""
    from v2.cfg_loader import load_bank_cfg
    from v2_analyze import _atomic_write

    cfg_dir = Path(cfg_dir)
    source = cfg_dir / "symbols.toml"
    if not source.is_file():
        return []
    symbols = load_symbols(source)
    by_bank = {}
    for symbol in symbols:
        by_bank.setdefault(symbol.bank, []).append(symbol)
    paths = {}
    for path in sorted(cfg_dir.glob("bank*.cfg")):
        match = _BANK.fullmatch(path.name)
        if match:
            bank = int(match[1], 16)
            if bank in paths:
                raise ValueError(f"{cfg_dir}: multiple cfg files for bank ${bank:02X}")
            paths[bank] = path
    for bank in by_bank:
        paths.setdefault(bank, cfg_dir / f"bank{bank:02x}.cfg")

    pending = []
    for bank, path in sorted(paths.items()):
        original = path.read_text(encoding="utf-8") if path.exists() else f"bank = {bank:02x}\n"
        prefix, suffix, managed = _split_block(original, path)
        if bank not in by_bank and not managed:
            continue
        # Load only the hand-owned part to avoid duplicating declarations or
        # losing entry/end overrides on a function already configured there.
        # cfg_loader accepts a file, so use a temporary sibling-free directory.
        with tempfile.TemporaryDirectory(prefix="snesrecomp-symbols-") as tmp:
            manual = Path(tmp) / path.name
            manual.write_text(prefix + suffix, encoding="utf-8")
            cfg = load_bank_cfg(str(manual))
        if cfg.bank != bank:
            raise ValueError(f"{path}: bank field does not match filename")
        entries = {entry.start: entry for entry in cfg.entries}
        forced = set(cfg.force_lle)
        lines = [BEGIN]
        for symbol in by_bank.get(bank, []):
            if symbol.emit and symbol.addr not in entries:
                lines.append(f"func {symbol.name} {symbol.addr:04x} "
                             f"entry_mx:{symbol.entry_m},{symbol.entry_x}")
                entries[symbol.addr] = symbol
            else:
                lines.append(f"symbol {symbol.pc24:06x} {symbol.name}")
            if not symbol.emit and symbol.pc24 not in forced:
                lines.append(f"force_lle {symbol.pc24:06x}")
                forced.add(symbol.pc24)
        lines.append(END)
        updated = prefix + "\n".join(lines) + "\n" + suffix
        if updated != original:
            pending.append((path, updated))
    for path, updated in pending:
        _atomic_write(path, updated)
    print(f"symbols: synchronized {len(symbols)} function labels "
          f"({sum(s.emit for s in symbols)} AOT requests) from {source}")
    return symbols
