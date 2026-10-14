#!/usr/bin/env python3
"""Loopback IPP tests. This is not the Preview/browser milestone."""
import base64
import contextlib
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import decode_fastraster

def attribute(tag, name, value):
    name = name.encode()
    if tag in (0x21, 0x23): value = struct.pack(">i", value)
    elif tag == 0x22: value = bytes([int(value)])
    elif isinstance(value, str): value = value.encode()
    return bytes([tag]) + struct.pack(">H", len(name)) + name + struct.pack(">H", len(value)) + value

def parse(data):
    assert len(data) >= 9, "truncated IPP response"
    status = struct.unpack(">H", data[2:4])[0]
    values, p, name = {}, 8, ""
    while p < len(data):
        tag = data[p]; p += 1
        if tag == 3: break
        if tag <= 0x0f: continue
        n = struct.unpack(">H", data[p:p+2])[0]; p += 2
        if n: name = data[p:p+n].decode(); p += n
        n = struct.unpack(">H", data[p:p+2])[0]; p += 2
        value = data[p:p+n]; p += n
        if tag in (0x21, 0x23) and n == 4: value = struct.unpack(">i", value)[0]
        elif tag == 0x22 and n == 1: value = bool(value[0])
        elif tag in (0x41, 0x42, 0x44, 0x45, 0x47, 0x48, 0x49): value = value.decode(errors="replace")
        values.setdefault(name, []).append(value)
    return status, values

class Client:
    def __init__(self, port, state=None):
        self.port = port
        self.state = state
        self.uri = f"ipp://127.0.0.1:{port}/ipp/print"
    def headers(self):
        headers = {"Content-Type": "application/ipp"}
        if self.state:
            token = (self.state / "access-token").read_text()
            headers["Authorization"] = "Basic " + base64.b64encode(("hp1010:" + token).encode()).decode()
        return headers
    def call(self, op, operation=(), job=(), document=b"", timeout=30):
        body = b"\x02\x00" + struct.pack(">HI", op, 1) + b"\x01"
        body += attribute(0x47, "attributes-charset", "utf-8")
        body += attribute(0x48, "attributes-natural-language", "en")
        body += attribute(0x45, "printer-uri", self.uri)
        for tag, name, value in operation: body += attribute(tag, name, value)
        if job:
            body += b"\x02"
            for tag, name, value in job: body += attribute(tag, name, value)
        body += b"\x03" + document
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        connection.request("POST", "/ipp/print", body, self.headers())
        response = connection.getresponse()
        result = response.read()
        connection.close()
        assert response.status == 200, (response.status, result[:100])
        return parse(result)
    def submit(self, document, copies=1, format="image/pwg-raster"):
        return self.call(2, [(0x49, "document-format", format), (0x42, "job-name", "private-title-sentinel")],
                         [(0x21, "copies", copies), (0x44, "media", "iso_a4_210x297mm")], document)
    def job(self, job_id):
        return self.call(9, [(0x21, "job-id", job_id)])
    def wait(self, job_id):
        for _ in range(150):
            code, attrs = self.job(job_id)
            assert code < 0x100, (code, attrs)
            state = attrs["job-state"][0]
            if state >= 7: return state
            time.sleep(.1)
        raise AssertionError("job did not become terminal")

class Service:
    def __init__(self, state, extra=()):
        self.state = state
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0)); self.port = s.getsockname()[1]
        self.client = Client(self.port, self.state)
        self.extra = extra
    def __enter__(self):
        self.state.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.log = open(self.state / "test-stderr.log", "wb")
        os.chmod(self.state / "test-stderr.log", 0o600)
        self.process = subprocess.Popen([str(ROOT / "build/hp1010-helper"), "--simulate", "--state-dir", str(self.state),
                                         "--port", str(self.port), *self.extra], stdout=subprocess.DEVNULL, stderr=self.log)
        for _ in range(100):
            if self.process.poll() is not None:
                self.log.flush()
                raise AssertionError(("helper exited", (self.state / "test-stderr.log").read_text()))
            try:
                code, attrs = self.client.call(11)
                if code < 0x100: return self
            except OSError: pass
            time.sleep(.1)
        raise AssertionError("service did not listen")
    def __exit__(self, *exc):
        self.process.terminate()
        try: self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill(); self.process.wait(); raise AssertionError("helper stop timed out")
        self.log.close()
    def captures(self): return list((self.state / "captures").glob("*.fr"))
    def partials(self): return list((self.state / "captures").glob("*.partial"))

def fixture(path, width=4960, height=7016, pages=2, bits=1, pattern="sparse"):
    subprocess.run([str(ROOT / "build/tests/make-raster"), str(path), str(width), str(height), str(pages), str(bits), "pwg", pattern], check=True)
    return path.read_bytes()

def main():
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", str(ROOT / "tools/make-raster.c"), "-lcups", "-o", str(ROOT / "build/tests/make-raster")], check=True)
    with tempfile.TemporaryDirectory(prefix="hp1010-integration-") as work:
        work = Path(work)
        document = fixture(work / "pattern.pwg")
        state = work / "normal"
        with Service(state) as service:
            code, attrs = service.client.call(11)
            assert code < 0x100
            formats = attrs["document-format-supported"]
            assert "image/pwg-raster" in formats and "image/urf" in formats
            assert "application/pdf" not in formats and "text/plain" not in formats
            assert attrs["sides-supported"] == ["one-sided"]
            code, attrs = service.client.submit(document)
            assert code < 0x100, (code, attrs)
            assert service.client.wait(attrs["job-id"][0]) == 9
            assert len(service.captures()) == 1
            # The separate decoder must prove page count, geometry and pixels.
            decoded = decode_fastraster.decode(service.captures()[0].read_bytes())
            assert len(decoded) == 1 and len(decoded[0]["pages"]) == 2
            pages = decoded[0]["pages"]
            for index, page in enumerate(pages):
                assert page["width"] == 4992 and page["height"] == 7016 and page["media"] == "A4"
                assert all(strip["compression"] == 24 for strip in page["strips"])
                expected = bytearray(624)
                expected[5:15] = b"\xff" * 10
                assert page["rows"][50 + index * 50] == bytes(expected)
                assert sum(sum(decode_fastraster.BLACK_BITS[b] for b in row) for row in page["rows"]) == 80
            code, attrs = service.client.submit(document, copies=2)
            assert code < 0x100 and service.client.wait(attrs["job-id"][0]) == 9
            assert len(service.captures()) == 2
            newest = max(service.captures(), key=lambda p: p.stat().st_mtime_ns)
            copied = decode_fastraster.decode(newest.read_bytes())
            assert len(copied) == 2 and all(len(j["pages"]) == 2 for j in copied)
            assert copied[0]["pages"][0]["rows"] == copied[1]["pages"][0]["rows"]
            # Reject unsupported formats and malformed/truncated raster.
            code, attrs = service.client.submit(b"%PDF-1.7\n", format="application/pdf")
            assert code >= 0x400
            code, attrs = service.client.submit(document[:2000])
            if code < 0x100: assert service.client.wait(attrs["job-id"][0]) == 8
            code, attrs = service.client.submit(fixture(work / "oversize.pwg", 7101, 1, 1))
            if code < 0x100: assert service.client.wait(attrs["job-id"][0]) == 8
            assert len(service.captures()) == 2
            assert not [p for d in state.glob("spool-*") for p in d.iterdir()], "temporary job documents retained"
            for p in state.rglob("*"):
                if p.is_file(): assert p.stat().st_mode & 0o077 == 0, p
            assert "private-title-sentinel" not in (state / "test-stderr.log").read_text()
            print("PASS IPP capabilities, multi-page jobs, copies, malformed/bounds, privacy and cleanup")
        count = len(list((state / "captures").glob("*.fr")))
        with Service(state) as service:
            time.sleep(.3)
            assert len(service.captures()) == count, "restart replayed a document"
            print("PASS restart does not replay")
        with Service(work / "failure", ["--fail-after", "500"]) as service:
            code, attrs = service.client.submit(document)
            assert code < 0x100 and service.client.wait(attrs["job-id"][0]) == 8
            assert not service.captures() and len(service.partials()) == 1
            assert service.partials()[0].stat().st_size == 500
            report = json.loads((service.state / "state.json").read_text())
            assert "Simulated transport failure" in report["message"] and "do not resubmit" in report["message"]
            print("PASS partial transfer aborts and stays partial")
        with Service(work / "cancel", ["--max-write", "4096", "--delay-ms", "100"]) as service:
            code, attrs = service.client.call(5, [], [(0x44, "media", "iso_a4_210x297mm")])
            assert code < 0x100
            job_id = attrs["job-id"][0]
            errors = []
            def send():
                try: service.client.call(6, [(0x21, "job-id", job_id), (0x49, "document-format", "image/pwg-raster"), (0x22, "last-document", True)], document=document)
                except Exception as e: errors.append(e)
            worker = threading.Thread(target=send); worker.start()
            for _ in range(100):
                if service.partials() and service.partials()[0].stat().st_size > 500: break
                time.sleep(.05)
            code, attrs = service.client.call(8, [(0x21, "job-id", job_id)])
            assert code < 0x100, (code, attrs)
            worker.join(10); assert not worker.is_alive() and not errors, errors
            assert service.client.wait(job_id) == 7
            assert not service.captures()
            print("PASS in-flight cancellation with no completed capture")
    print("All software IPP checks passed; physical printer remains untested")

if __name__ == "__main__": main()
