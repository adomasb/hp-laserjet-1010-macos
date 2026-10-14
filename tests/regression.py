#!/usr/bin/env python3
"""Adversarial raster and process-lifetime regressions; no hardware needed."""
import http.client
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from integration import ROOT, Client, Service, attribute, fixture, parse


def accepted(client, document, expected, format="image/pwg-raster", copies=1):
    code, attrs = client.submit(document, copies=copies, format=format)
    assert code < 0x100, (code, attrs)
    assert client.wait(attrs["job-id"][0]) == expected


def guards(work):
    document = fixture(work / "small.pwg", 128, 129, 1)
    with Service(work / "guard") as service:
        for count in (0, 1):
            base = bytearray(document)
            struct.pack_into(">I", base, 4 + 452, count)
            accepted(service.client, base, 9)
            for size in (1, 15, 16, 31, 32, 96, 1795):
                accepted(service.client, base + document[4:4 + size], 8)
        # Fields that libcups otherwise uses to allocate before caller validation.
        for offset, value in ((392, 0x40000000), (372, 0xffffffff),
                              (376, 0xffffffff), (388, 0xffffffff), (420, 0xffffffff)):
            hostile = bytearray(document)
            struct.pack_into(">I", hostile, 4 + offset, value)
            accepted(service.client, hostile, 8)
        body = b"\x02\x00" + struct.pack(">HI", 2, 1) + b"\x01"
        body += attribute(0x47, "attributes-charset", "utf-8")
        body += attribute(0x48, "attributes-natural-language", "en")
        body += attribute(0x45, "printer-uri", service.client.uri)
        body += attribute(0x49, "document-format", "image/pwg-raster") + b"\x03" + document
        connection = http.client.HTTPConnection("127.0.0.1", service.port, timeout=30)
        connection.putrequest("POST", "/ipp/print")
        for name, value in service.client.headers().items(): connection.putheader(name, value)
        connection.putheader("Transfer-Encoding", "chunked")
        connection.endheaders()
        # One-byte HTTP chunks exercise fragmentation inside IPP and raster fields.
        for byte in body:
            connection.send(b"1\r\n" + bytes([byte]) + b"\r\n")
        connection.send(b"0\r\n\r\n")
        response = connection.getresponse()
        assert response.status == 200
        code, attrs = parse(response.read())
        connection.close()
        assert code < 0x100 and service.client.wait(attrs["job-id"][0]) == 9
        assert len(service.captures()) == 3
    print("PASS PWG clean EOF, partial headers, hostile allocation fields, one-byte HTTP chunks")

    path = work / "small.urf"
    subprocess.run([str(ROOT / "build/tests/make-raster"), str(path), "128", "129", "2", "8", "urf"], check=True)
    urf = path.read_bytes()
    with Service(work / "urf") as service:
        accepted(service.client, urf, 9, "image/urf")
        from decode_fastraster import decode, BLACK_BITS
        pages = decode(service.captures()[0].read_bytes())[0]["pages"]
        assert len(pages) == 2
        for page in pages:
            assert sum(BLACK_BITS[b] for row in page["rows"] for b in row) == 80
        for size in (1, 15, 16, 31):
            accepted(service.client, urf + urf[12:12 + size], 8, "image/urf")
        for offset, value in ((12, 0xffffffff), (16, 0xffffffff), (20, 300)):
            hostile = bytearray(urf)
            struct.pack_into(">I", hostile, 12 + offset, value)
            accepted(service.client, hostile, 8, "image/urf")
        assert len(service.captures()) == 1
    print("PASS URF gray8 exact pixels, partial headers, dimensions and resolution bounds")

    for fault in ("io", "timeout", "disconnect"):
        with Service(work / fault, ["--failure", fault, "--fail-after", "500"]) as service:
            accepted(service.client, document, 8)
            assert not service.captures() and len(service.partials()) == 1
            assert service.partials()[0].stat().st_size == 500
            report = json.loads((service.state / "state.json").read_text())
            assert ("failure" if fault == "io" else fault) in report["message"]
            assert "do not resubmit" in report["message"]
    large = fixture(work / "a4.pwg", pages=1, pattern="noise")
    with Service(work / "copy-bound") as service:
        accepted(service.client, large, 8, copies=99)
        assert not service.captures()
        assert all(p.stat().st_size == 0 for p in service.partials())
    print("PASS I/O, timeout, disconnect abort at exact prefix; aggregate copies bound before transfer")
    return large


def parent_lifecycle(work, document):
    state = work / "parent"
    state.mkdir(mode=0o700)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    code = """import os, subprocess, sys, time
subprocess.Popen([sys.argv[1], '--simulate', '--state-dir', sys.argv[2], '--port',
                  sys.argv[3], '--parent-pid', str(os.getpid()), '--max-write', '4096',
                  '--delay-ms', '10'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
while True: time.sleep(1)
"""
    parent = subprocess.Popen([sys.executable, "-c", code, str(ROOT / "build/hp1010-helper"), str(state), str(port)])
    client = Client(port, state)
    worker = None
    try:
        for _ in range(100):
            try:
                if client.call(11)[0] < 0x100: break
            except OSError: pass
            time.sleep(.05)
        else: raise AssertionError("owned helper did not listen")
        # The killed parent's child may close HTTP without a final response.
        def submit():
            try: client.submit(document)
            except (OSError, http.client.HTTPException): pass
        worker = threading.Thread(target=submit)
        worker.start()
        for _ in range(100):
            partials = list((state / "captures").glob("*.partial"))
            if partials and partials[0].stat().st_size > 500: break
            time.sleep(.05)
        else: raise AssertionError("no active transfer")
        parent.kill()
        parent.wait(timeout=5)
        for _ in range(200):
            report = json.loads((state / "state.json").read_text())
            if report["service"] == "stopped": break
            time.sleep(.05)
        else: raise AssertionError("orphan helper did not stop")
        assert not list(state.glob("spool-*"))
        assert not list((state / "captures").glob("*.fr"))
        worker.join(10)
        assert not worker.is_alive()
        # Reuse the same address immediately; restart never replays the partial job.
        process = subprocess.Popen([str(ROOT / "build/hp1010-helper"), "--simulate", "--state-dir", str(state), "--port", str(port)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                try:
                    if client.call(11)[0] < 0x100: break
                except OSError: pass
                time.sleep(.05)
            else: raise AssertionError("same-port relaunch failed")
            time.sleep(.3)
            assert not list((state / "captures").glob("*.fr"))
        finally:
            process.terminate()
            process.wait(timeout=10)
    finally:
        if parent.poll() is None:
            parent.kill()
            parent.wait(timeout=5)
        if worker is not None: worker.join(10)
    print("PASS parent SIGKILL cancels transfer, stops listener, removes spool, relaunches without replay")


if __name__ == "__main__":
    (ROOT / "build/tests").mkdir(parents=True, exist_ok=True)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", str(ROOT / "tools/make-raster.c"), "-lcups",
                    "-o", str(ROOT / "build/tests/make-raster")], check=True)
    with tempfile.TemporaryDirectory(prefix="hp1010-regression-") as temporary:
        work = Path(temporary)
        document = guards(work)
        parent_lifecycle(work, document)
