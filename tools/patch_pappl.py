#!/usr/bin/env python3
"""Apply reviewed PAPPL patches only to the exact expected upstream files."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / 'patches/pappl'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def apply(source):
    manifest = json.loads((PATCHES / 'manifest.json').read_text())
    additions = {name: PATCHES / name for name in ('hp-input.c', 'hp-raster.c', 'hp-raster.h')}
    additions['printer_protocol.h'] = ROOT / 'src/printer_protocol.h'
    # Validate the whole input before changing any file; never allow patch fuzz,
    # already-applied patches, or a half-patched dependency to pass unnoticed.
    for name, hashes in manifest.items():
        if digest(source / name) != hashes['before']:
            raise ValueError(f'PAPPL patch input mismatch: {name}')
    for name in additions:
        if (source / 'pappl' / name).exists():
            raise ValueError(f'PAPPL adapter destination already exists: {name}')
    commands = [['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-i', str(PATCHES / name)]
                for name in ('multipart.patch', 'integration.patch')]
    for command in commands:
        subprocess.run([*command, '--dry-run'], cwd=source, check=True, stdout=subprocess.PIPE)
    for command in commands:
        subprocess.run(command, cwd=source, check=True, stdout=subprocess.PIPE)
    for name, hashes in manifest.items():
        if digest(source / name) != hashes['after']:
            raise ValueError(f'PAPPL patch output mismatch: {name}')
    for name, path in additions.items():
        shutil.copyfile(path, source / 'pappl' / name)


if __name__ == '__main__':
    try:
        apply(Path(sys.argv[1]).resolve())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'Cannot patch PAPPL: {error}')
