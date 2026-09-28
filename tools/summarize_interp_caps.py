#!/usr/bin/env python3
"""Summarize existing interpreter-cap logs without changing guest execution."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

FIELDS = re.compile(r'\b(entry|last|op|m|x|db|sp|a|flag)=([^\s]+)')


def summarize(text: str) -> dict:
    counts = Counter()
    invalid = []
    total = zero = 0
    for line in text.splitlines():
        if '[interp_cap]' not in line or 'head:' in line:
            continue
        try:
            fields = {key: int(value.lstrip('\\$'), 16)
                      for key, value in FIELDS.findall(line)}
            if not {'entry', 'last', 'op', 'm', 'x', 'sp'} <= fields.keys():
                raise ValueError('missing cap fields')
        except ValueError:
            invalid.append(line)
            continue
        counts[tuple(sorted(fields.items()))] += 1
        total += 1
        zero += fields['entry'] == 0
    return {
        'schema': 'snesrecomp interp-cap summary v1',
        'cap_count': total,
        'zero_entry_count': zero,
        'records': [dict(fields=dict(fields), count=count)
                    for fields, count in sorted(counts.items())],
        'unparsed_cap_count': len(invalid),
        'unparsed_examples': invalid[:5],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--engine-revision', default='unknown')
    parser.add_argument('--game-revision', default='unknown')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    raw = args.log.read_bytes()
    result = summarize(raw.decode('utf-8', errors='replace'))
    result.update(log_sha256=hashlib.sha256(raw).hexdigest(),
                  engine_revision=args.engine_revision,
                  game_revision=args.game_revision)
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(output, encoding='utf-8')
    else:
        print(output, end='')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
