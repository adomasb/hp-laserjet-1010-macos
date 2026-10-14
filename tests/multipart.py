#!/usr/bin/env python3
"""Exercise actual PAPPL multipart parsing, including the upstream DoS trigger."""
import http.client
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([str(ROOT / 'scripts/build-deps.sh')], check=True)
sys.path.insert(0, str(ROOT / 'tools'))
from dependencies import dependency_path
pappl = dependency_path('pappl')
openssl = dependency_path('openssl')
with tempfile.TemporaryDirectory(prefix='hp1010-multipart-') as temporary:
    temporary = Path(temporary)
    executable = temporary / 'test-multipart'
    # Compile the vulnerable translation unit itself with ASan/UBSan, not just
    # the harness. The linker uses it instead of the archive's uninstrumented one.
    subprocess.run(['clang', '-std=c11', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-Wno-deprecated-declarations',
                    '-I', str(pappl), str(ROOT / 'tests/test_multipart.c'),
                    str(pappl / 'pappl/client-webif.c'), str(pappl / 'pappl/libpappl.a'),
                    str(openssl / 'libssl.a'), str(openssl / 'libcrypto.a'),
                    '-lcups', '-lz', '-lpthread', '-framework', 'AppKit', '-framework', 'IOKit',
                    '-framework', 'CoreFoundation', '-o', str(executable)], check=True)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    with (temporary / 'stderr').open('wb') as log:
        child = subprocess.Popen([str(executable), str(port), str(temporary)], stderr=log,
                                 stdout=subprocess.DEVNULL)
        def request(body, boundary='test-boundary'):
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
            try:
                connection.request('POST', '/form', body, {'Content-Type': f'multipart/form-data; boundary={boundary}'})
                response = connection.getresponse()
                response.read()
                assert response.status in (400, 404), response.status
                return int(response.status == 400)
            finally:
                connection.close()
        try:
            for _ in range(100):
                assert child.poll() is None, 'parser test server exited'
                try:
                    request(b'x')
                    break
                except ConnectionRefusedError as error:
                    last_error = str(error)
                    time.sleep(.05)
            else:
                raise AssertionError(f'parser test server did not start: {last_error}')
            prefix = b'--test-boundary\r\nContent-Disposition: form-data; name="x"\r\n\r\n'
            valid = prefix + b'a\r\n--test-boundary--\r\n'
            assert request(valid) == 1
            # Missing closing delimiter, including the reported one-byte case.
            for suffix in (b'', b'a', b'\r', b'\r\n', b'\r' * 128, b'x' * 128):
                assert request(prefix + suffix) == 0
            assert request(b'a') == 0
            assert request(valid, boundary='') == 0
            assert request(b'x' * (2 * 1024 * 1024)) == 0
            assert request(valid) == 1, 'parser failed to recover'
            assert child.poll() is None
        except Exception:
            print((temporary / 'stderr').read_text()[-6000:])
            raise
        finally:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    diagnostics = (temporary / 'stderr').read_text()
    assert 'Sanitizer' not in diagnostics and 'runtime error:' not in diagnostics, diagnostics
print('PASS multipart valid input, missing/short boundaries, size cap, recovery; ASan/UBSan enabled')
