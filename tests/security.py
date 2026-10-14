#!/usr/bin/env python3
"""Local HTTP/IPP trust boundary and macOS libcups authentication, without USB."""
import base64
import http.client
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
from integration import ROOT, Service, attribute, fixture, parse


def body(client, op, attrs=(), document=b""):
    result = b"\x02\x00" + struct.pack(">HI", op, 1) + b"\x01"
    for tag, name, value in [(0x47, "attributes-charset", "utf-8"),
                             (0x48, "attributes-natural-language", "en"),
                             (0x45, "printer-uri", client.uri), *attrs]:
        result += attribute(tag, name, value)
    return result + b"\x03" + document


def request(service, data, headers=None, method="POST", path="/ipp/print", connection=None):
    own = connection is None
    connection = connection or http.client.HTTPConnection("127.0.0.1", service.port, timeout=8)
    connection.request(method, path, data, headers or {"Content-Type": "application/ipp"})
    response = connection.getresponse()
    result = (response.status, dict(response.getheaders()), response.read())
    if own: connection.close()
    return result


def main():
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", str(ROOT / "tools/make-raster.c"),
                    "-lcups", "-o", str(ROOT / "build/tests/make-raster")], check=True)
    with tempfile.TemporaryDirectory(prefix="hp1010-security-") as temporary:
        work = Path(temporary)
        raster = fixture(work / "page.pwg", 128, 129, 1)
        state = work / "service"
        with Service(state) as service:
            client = service.client
            capability = body(client, 11)
            public = request(service, capability)
            assert public[0] == 200
            assert parse(public[2])[1]["uri-authentication-supported"] == ["basic"]
            # Exact allowlist, including formerly accepted numeric prefixes and .local names.
            for host in (f"127.0.0.1:{service.port}", f"localhost:{service.port}", f"LOCALHOST:{service.port}"):
                assert request(service, capability, {"Content-Type": "application/ipp", "Host": host})[0] == 200
            for host in ("attacker.example", "1attacker.example", "unrelated.local", "unrelated.local.",
                         "[attacker]", "127.0.0.2", "127.0.0.1", "localhost", "127.1", "2130706433",
                         f"127.0.0.1:{service.port + 1}", f"127.0.0.1:+{service.port}",
                         f"127.0.0.1:{service.port}junk", f"127.0.0.1:{service.port},attacker.example",
                         f"localhost.attacker.example:{service.port}", ""):
                assert request(service, capability, {"Content-Type": "application/ipp", "Host": host})[0] == 400, host
            for version in ("1.0", "1.1"):
                with socket.create_connection(("127.0.0.1", service.port), timeout=8) as sock:
                    sock.sendall(f"POST /ipp/print HTTP/{version}\r\nContent-Length: 0\r\n\r\n".encode())
                    assert b" 400 " in sock.recv(1024).split(b"\r\n")[0]
            assert request(service, capability, path=f"http://attacker.example:{service.port}/ipp/print")[0] == 400
            for method, path, content_type in (("GET", "/", ""), ("GET", "/ipp/print", ""),
                    ("OPTIONS", "/ipp/print", "application/ipp"), ("POST", "/ipp/print", "text/plain"),
                    ("POST", "/ipp/print", "application/x-www-form-urlencoded"),
                    ("POST", "/ipp/print", "multipart/form-data; boundary=abc"),
                    ("POST", "/ipp/print?ignored=1", "application/ipp")):
                result = request(service, capability, {"Content-Type": content_type,
                    "Origin": "https://attacker.example", "Access-Control-Request-Method": "POST"}, method, path)
                assert result[0] == 403, (method, path, result[0])
                assert not any(name.lower().startswith("access-control-allow") for name in result[1])

            token = (state / "access-token").read_text()
            # Retain an actual job to test disclosure and cancellation, including a spoofed username.
            code, attrs = client.submit(raster)
            assert code < 0x100 and client.wait(attrs["job-id"][0]) == 9
            job_id = attrs["job-id"][0]
            for op in (2, 4, 5, 6, 8, 9, 10, 16, 17, 18, 19, 0x22, 0x38, 0x4001):
                payload = body(client, op, [(0x21, "job-id", job_id), (0x42, "requesting-user-name", "hp1010")])
                for authorization in (None, "Basic " + base64.b64encode(b"hp1010:wrong").decode(),
                                      "Bearer " + token,
                                      "Basic " + base64.b64encode(("other:" + token).encode()).decode()):
                    headers = {"Content-Type": "application/ipp"}
                    if authorization: headers["Authorization"] = authorization
                    result = request(service, payload, headers)
                    assert result[0] == 401 and "Basic" in result[1].get("WWW-Authenticate", ""), (op, result[0])
                    assert b"private-title-sentinel" not in result[2]
            assert len(service.captures()) == 1
            assert client.call(11)[1]["printer-state"] == [3]
            # Successful authentication must not carry into the next keep-alive request.
            connection = http.client.HTTPConnection("127.0.0.1", service.port, timeout=8)
            assert request(service, body(client, 10), client.headers(), connection=connection)[0] == 200
            assert request(service, body(client, 10), connection=connection)[0] == 401
            connection.close()
            for op in (16, 17): assert client.call(op)[0] < 0x100
            print("PASS exact Host/port validation, browser/form rejection, all-operation authentication, spoofed user and keep-alive isolation")

            # Exercise the system CUPS library's actual 401 challenge/retry path.
            result = subprocess.run([str(ROOT / "build/tests/test-auth-client"), str(service.port),
                                     str(state), str(work / "page.pwg")], capture_output=True, timeout=30)
            assert token.encode() not in result.stdout + result.stderr
            assert result.returncode == 0, (result.returncode, result.stderr.decode(errors="replace")[-2000:])
            assert len(service.captures()) == 2
            assert service.captures()[0].read_bytes() == service.captures()[1].read_bytes()
            for name in ("state.json", "test-stderr.log"):
                assert token.encode() not in (state / name).read_bytes()
            print("PASS macOS libcups authenticates and prints identical simulated output without leaking credentials")
        with Service(state) as restarted:
            assert (state / "access-token").read_text() == token
            assert restarted.client.call(10)[0] < 0x100
            assert not any(state.glob("spool-*/job-*"))
        print("PASS restart retains credential; no persisted job replay")


if __name__ == "__main__": main()
