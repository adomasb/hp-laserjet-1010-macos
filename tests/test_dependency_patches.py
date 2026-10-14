#!/usr/bin/env python3
"""Verify exact-source patch application and fail-closed upgrade behavior."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from dependencies import read_dependencies
import patch_pappl


class PatchTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='hp1010-patch-test-')
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name)
        self.manifest = json.loads((patch_pappl.PATCHES / 'manifest.json').read_text())
        pin = read_dependencies()['pappl']
        archive = ROOT / '.cache' / f"pappl-{pin['version']}.tar.gz"
        self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), pin['sha256'])
        # Read only known regular source files; do not extract archive paths.
        with tarfile.open(archive) as tar:
            for name in self.manifest:
                path = self.source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(tar.extractfile(f"pappl-{pin['version']}/{name}").read())

    def test_verified_source_applies_and_cannot_apply_twice(self):
        patch_pappl.apply(self.source)
        for name, hashes in self.manifest.items():
            self.assertEqual(patch_pappl.digest(self.source / name), hashes['after'])
        self.assertTrue((self.source / 'pappl/hp-raster.c').is_file())
        self.assertEqual((self.source / 'pappl/printer_protocol.h').read_bytes(),
                         (ROOT / 'src/printer_protocol.h').read_bytes())
        with self.assertRaisesRegex(ValueError, 'input mismatch'):
            patch_pappl.apply(self.source)

    def test_changed_upstream_fails_before_any_edits_even_with_python_optimization(self):
        path = self.source / 'pappl/job-process.c'
        path.write_bytes(path.read_bytes() + b'\n/* Unexpected upstream change. */\n')
        before = {name: (self.source / name).read_bytes() for name in self.manifest}
        result = subprocess.run([sys.executable, '-O', str(ROOT / 'tools/patch_pappl.py'), str(self.source)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('input mismatch', result.stderr)
        self.assertEqual(before, {name: (self.source / name).read_bytes() for name in self.manifest})
        self.assertFalse((self.source / 'pappl/hp-raster.c').exists())

    def test_adapter_collision_is_rejected_before_edits(self):
        (self.source / 'pappl/hp-raster.c').write_text('unexpected source')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            patch_pappl.apply(self.source)
        for name, hashes in self.manifest.items():
            self.assertEqual(patch_pappl.digest(self.source / name), hashes['before'])


if __name__ == '__main__':
    unittest.main()
