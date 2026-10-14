#!/usr/bin/env python3
"""Shared dependency pins for shell builds, packaging, and tests."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read_dependencies(root=ROOT):
    pins = json.loads((root / 'config/dependencies.json').read_text())
    if set(pins) != {'pappl', 'openssl'}:
        raise ValueError('Expected PAPPL and OpenSSL dependency pins.')
    for name, pin in pins.items():
        if (set(pin) != {'version', 'sha256'} or
                not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', pin['version']) or
                not re.fullmatch(r'[0-9a-f]{64}', pin['sha256'])):
            raise ValueError(f'Invalid dependency pin: {name}')
    return pins


def dependency_path(name, root=ROOT):
    return root / 'build/deps' / f"{name}-{read_dependencies(root)[name]['version']}"


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name', choices=('pappl', 'openssl'))
    parser.add_argument('field', choices=('version', 'sha256', 'path'))
    args = parser.parse_args()
    print(dependency_path(args.name) if args.field == 'path' else read_dependencies()[args.name][args.field])
