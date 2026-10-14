#!/usr/bin/env python3
"""Verify the packaging gate catches the metadata that ordinary strings missed."""
import importlib.util
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('privacy', root / 'tools/check-release-privacy.py')
privacy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(privacy)
assert privacy.inspect('helper', b'\0' + b'/' + b'Users/example/project/source.c\0')
assert privacy.inspect('helper', b'\0' + b'/' + b'private/var/' + b'folders/ab/random/T/build\0')
assert privacy.inspect('config', b'gh' + b'p_' + b'x' * 36)
assert privacy.inspect('config', b'user@host.' + b'lan')
license_data = (root / 'LICENSE').read_bytes()
assert not privacy.inspect('LICENSE', license_data)
assert not privacy.inspect('Contents/Resources/Licences/HP1010-MIT-LICENSE.txt', license_data)
author = re.search(rb'^Copyright \(c\) \d{4} (.+)$', license_data, re.M)[1]
assert privacy.inspect('source.c', author)
assert not privacy.inspect('source.c', b'int main(void) { return 0; }')
try:
    privacy.check([('.git/config', b'')])
except ValueError:
    pass
else:
    raise AssertionError('Git metadata was allowed')
print('PASS privacy gate detects binary paths, personal identifiers, tokens and Git metadata; license allowed')
