#!/usr/bin/env python3
"""Export only reviewed public files, without Git history or local artifacts."""
import importlib.util
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("privacy", ROOT / "tools/check-release-privacy.py")
privacy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(privacy)
entries = list(privacy.public_files())
privacy.check(entries)
destination = ROOT / "build/LaserJet1010Source.zip"
destination.parent.mkdir(exist_ok=True)
temporary = destination.with_suffix('.zip.partial')
try:
    with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries:
            entry = zipfile.ZipInfo('hp-laserjet-1010-macos/' + name, (2026, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = (0o100755 if name.endswith('.sh') else 0o100644) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data)
    with zipfile.ZipFile(temporary) as archive:
        privacy.check((name, archive.read(name)) for name in archive.namelist())
    temporary.replace(destination)
finally:
    temporary.unlink(missing_ok=True)
print('Created build/LaserJet1010Source.zip (no history, caches, or print data)')
