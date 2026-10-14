#!/usr/bin/env python3
"""Independent strict parser for this project's HP 1010 FastRaster profiles.

Byte values and hand fixture are traced to HPLIP 3.26.6, not obtained from
the encoder. No device behaviour is simulated. See THIRD_PARTY_NOTICES.md for reference sources.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path

MAX_BYTES = 128 * 1024 * 1024
MAX_DECODED_BYTES = 128 * 1024 * 1024
MAX_STRIP_BYTES = 128 * (4 * 888 + 8)
BLACK_BITS = tuple(bin(value).count("1") for value in range(256))
UEL = bytes.fromhex("1b252d313233343558")
SESSION = bytes.fromhex(
    "c000f886c003f88fd158025802f88941"
    "c211207068f89146c000f888c001f88248")
PAGE = bytes.fromhex("43d300000000f82a75c007f8036ac0ccf82c7bd300000000f84c6b")
STRIP = bytes.fromhex("c206207068f891c2")
PJL = (b"@PJL SET RESOLUTION=600\n@PJL SET BITSPERPIXEL=1\n"
       b"@PJL SET ECONOMODE=OFF\r\n@PJL SET TIMEOUT=900\n"
       b"@PJL ENTER LANGUAGE=PCLXL\n) HP-PCL XL;2;0;Comment\n")
END = b"\x49\x42" + UEL + b"@PJL EOJ\n" + UEL


class ProtocolError(ValueError):
    pass


class Reader:
    def __init__(self, data):
        if len(data) > MAX_BYTES:
            raise ProtocolError("Capture exceeds 128 MiB limit")
        self.data, self.offset = data, 0

    def take(self, length):
        if length < 0 or length > len(self.data) - self.offset:
            raise ProtocolError(f"Truncated frame at byte {self.offset}")
        start = self.offset
        self.offset += length
        return self.data[start:self.offset]

    def expect(self, value):
        start = self.offset
        if self.take(len(value)) != value:
            raise ProtocolError(f"Invalid operator or framing at byte {start}")

    def has(self, value):
        return self.data.startswith(value, self.offset)


def delta_count(r, value, saturated):
    """HP's inclusive count escape (ljfastraster.cpp encode/decode_count)."""
    if value != saturated:
        return value
    extension = r.take(1)[0]
    if extension == 254:
        extension += r.take(1)[0]
    elif extension == 255:
        extension += int.from_bytes(r.take(2), "big")
        if extension < 510:
            raise ProtocolError("Noncanonical DeltaPlus count escape")
    return value + extension


def delta_rows(payload, stride, height):
    """Decode only explicit literals and HP's canonical seed-to-end command.

    Locations 1/2/3 predict pixels in the general HP algorithm; this project's
    conservative subset uses location 1 solely when a seed copy finishes a row.
    A literal count is additional bytes after the explicit first color, not the
    total length. Seed state resets to zero at each strip (HP constructor).
    """
    r = Reader(payload)
    seed, rows = bytes(stride), []
    for _ in range(height):
        row = bytearray()
        while len(row) < stride:
            command = r.take(1)[0]
            isrun, location = command >> 7, (command >> 5) & 3
            copied = delta_count(r, (command >> 3) & 3, 3)
            end = len(row) + copied
            if end > stride:
                raise ProtocolError("DeltaPlus seed copy exceeds row")
            row.extend(seed[len(row):end])
            if end == stride:
                if not isrun or location != 1 or command & 7:
                    raise ProtocolError("Invalid DeltaPlus seed-to-end command")
                break  # HP bypasses pixel prediction/count at the row boundary.
            if isrun or location:
                raise ProtocolError("Unsupported DeltaPlus run or prediction")
            first = r.take(1)  # Explicit color precedes replacement-count escape.
            additional = delta_count(r, command & 7, 7)
            if additional > 511 or additional + 1 > stride - len(row):
                raise ProtocolError("DeltaPlus literal exceeds row or 512-byte limit")
            row.extend(first)
            row.extend(r.take(additional))
        seed = bytes(row)
        rows.append(seed)
    if r.offset != len(payload):
        raise ProtocolError("Trailing DeltaPlus payload bytes")
    return rows


def decode(data):
    """Return jobs, with page rows retaining declared white stride padding.

    Original input pixelWidth is absent from the wire; width is the declared
    multiple-of-64 width. Never infer the original width from white columns.
    """
    r = Reader(data)
    jobs, decoded_bytes = [], 0
    while r.offset < len(data):
        r.expect(UEL + PJL + SESSION)
        pages = []
        while r.has(b"\xc0\x00\xf8\x28"):
            r.expect(b"\xc0\x00\xf8\x28\xc0")
            media = r.take(1)[0]
            if media not in (0, 2):
                raise ProtocolError("Unsupported FastRaster media enum")
            r.expect(b"\xf8\x25" + PAGE)
            rows, strips, width = [], [], None
            short_strip = False
            while r.has(STRIP):
                if short_strip:
                    raise ProtocolError("Strip follows a short final strip")
                r.expect(STRIP)
                outer_length = int.from_bytes(r.take(4), "little")
                r.expect(b"\xf8\x92\x46")
                h = r.take(25)
                # Parse numeric fields independently; no encoder calls/imports.
                dpi = int.from_bytes(h[2:4], "big")
                compression = int.from_bytes(h[4:6], "big")
                payload_size = int.from_bytes(h[12:16], "big")
                strip_width = int.from_bytes(h[16:18], "big")
                height = int.from_bytes(h[18:20], "big")
                x = int.from_bytes(h[20:22], "big")
                y = int.from_bytes(h[22:24], "big")
                if h[0:2] != b"\x00\x01" or h[6:12] != b"\x03\x00\x00\x00\x00\x00" or h[24] != 1:
                    raise ProtocolError("Invalid reserved/plane/depth header fields")
                if dpi != 600 or compression not in (2, 24):
                    raise ProtocolError("Only 600 dpi RAW 2/DeltaPlus 24 are implemented")
                if not 64 <= strip_width <= 7104 or strip_width % 64:
                    raise ProtocolError("Invalid byte-aligned FastRaster stride")
                stride = strip_width // 8
                if not 1 <= height <= 128 or x != 0 or y != len(rows) or y % 128:
                    raise ProtocolError("Invalid strip height or position")
                if len(rows) + height > 7100:
                    raise ProtocolError("Page exceeds raster height limit")
                if width is not None and width != strip_width:
                    raise ProtocolError("Width changes within a page")
                width = strip_width
                if payload_size > MAX_STRIP_BYTES or outer_length != 25 + payload_size:
                    raise ProtocolError("Invalid/oversized vendor-data length")
                decoded_bytes += stride * height
                if decoded_bytes > MAX_DECODED_BYTES:
                    raise ProtocolError("Decoded raster exceeds 128 MiB limit")
                if compression == 2 and payload_size != stride * 128:
                    raise ProtocolError("Invalid RAW payload length")
                payload = r.take(payload_size)
                if compression == 2:
                    if any(payload[height * stride:]):
                        raise ProtocolError("Final RAW strip padding is not white")
                    rows.extend(payload[i * stride:(i + 1) * stride] for i in range(height))
                else:
                    rows.extend(delta_rows(payload, stride, height))
                strips.append({"y": y, "height": height, "payload_bytes": payload_size,
                               "compression": compression})
                short_strip = height < 128
            if not rows:
                raise ProtocolError("Page contains no raster strips")
            r.expect(b"\x44")
            pages.append({"width": width, "height": len(rows),
                          "media": "A4" if media == 2 else "Letter",
                          "rows": rows, "strips": strips})
        if not pages:
            raise ProtocolError("Job contains no pages")
        r.expect(END)
        jobs.append({"pages": pages})
    if not jobs:
        raise ProtocolError("Capture is empty")
    return jobs


def summary(jobs):
    results = []
    for job in jobs:
        results.append({"pages": [
            {"width": p["width"], "height": p["height"], "media": p["media"],
             "strips": p["strips"],
             "black_pixels": sum(BLACK_BITS[b] for row in p["rows"] for b in row),
             "raster_sha256": hashlib.sha256(b"".join(p["rows"])).hexdigest()}
            for p in job["pages"]]})
    return {"jobs": results, "job_count": len(jobs),
            "page_count": sum(len(j["pages"]) for j in jobs)}


def self_test():
    # Literal independent 64-pixel, one-row source-derived RAW frame. Header
    # height=1, payload contains all128 rows as HP's fallback does, not a
    # guessed 8-byte final RAW payload. First and last pixel are black.
    frame = bytes.fromhex(
        "c206207068f891c219040000f89246"
        "00010258000203000000000000000400004000010000000001"
        "8000000000000001") + bytes(1016)
    fixture = UEL + PJL + SESSION + bytes.fromhex("c000f828c002f825") + PAGE + frame + b"\x44" + END
    result = decode(fixture)
    page = result[0]["pages"][0]
    assert page["width"] == 64 and page["height"] == 1
    assert page["rows"] == [bytes.fromhex("8000000000000001")]
    assert summary(result)["jobs"][0]["pages"][0]["black_pixels"] == 2
    assert len(decode(fixture + fixture)) == 2
    frame_offset = fixture.index(STRIP)
    header_offset = frame_offset + 15
    for offset in (frame_offset + 8, header_offset, header_offset + 4,
                   header_offset + 6, header_offset + 16, header_offset + 18,
                   header_offset + 22, header_offset + 24, header_offset + 25 + 8):
        broken = bytearray(fixture)
        broken[offset] ^= 1
        try:
            decode(broken)
        except ProtocolError:
            pass
        else:
            raise AssertionError(f"Mutation accepted at {offset}")
    for broken in (b"", fixture[:-1], fixture + b"junk"):
        try:
            decode(broken)
        except ProtocolError:
            pass
        else:
            raise AssertionError("Invalid/truncated fixture accepted")
    delta_self_test()


def delta_self_test():
    global MAX_DECODED_BYTES
    # Hand-written command/count bytes from HP's macros, never encoder output.
    # Saturation is inclusive: 3+253, 3+254+0, 3+254+255, 3+255+255.
    for count, escape in ((3, "00"), (256, "fd"), (257, "fe00"),
                          (512, "feff"), (513, "ff00ff"), (888, "ff0276")):
        assert delta_rows(b"\xb8" + bytes.fromhex(escape), count, 1) == [bytes(count)]
    for additional, header in ((0, "00aa"), (6, "06aa"), (7, "07aa00"),
                               (260, "07aafd"), (261, "07aafe00"),
                               (511, "07aafefa")):
        assert delta_rows(bytes.fromhex(header) + b"\xaa" * additional,
                          additional + 1, 1) == [b"\xaa" * (additional + 1)]
    # Seed escape, explicit first color, then replacement escape, in that order.
    assert delta_rows(bytes.fromhex("1f02aa00") + b"\xaa" * 7, 13, 1) == [
        bytes(5) + b"\xaa" * 8]
    # Full maximum stride split into explicit 512-byte and 376-byte literals.
    widest = bytes.fromhex("07aafefa") + b"\xaa" * 511
    widest += bytes.fromhex("07aafe72") + b"\xaa" * 375
    assert delta_rows(widest, 888, 1) == [b"\xaa" * 888]
    payload = bytes.fromhex("007fb804b8050000b804")
    expected = [b"\x7f" + bytes(7), b"\x7f" + bytes(7), bytes(8)]
    assert delta_rows(payload, 8, 3) == expected
    assert delta_rows(bytes.fromhex("b805"), 8, 1) == [bytes(8)]  # New-strip seed.

    def capture(body, declared=None):
        size = len(body) if declared is None else declared
        header = bytes.fromhex("000102580018030000000000") + size.to_bytes(4, "big")
        header += bytes.fromhex("004000030000000001")
        frame = STRIP + (25 + size).to_bytes(4, "little") + b"\xf8\x92\x46" + header + body
        return UEL + PJL + SESSION + bytes.fromhex("c000f828c002f825") + PAGE + frame + b"\x44" + END

    fixture = capture(payload)
    page = decode(fixture)[0]["pages"][0]
    assert page["rows"] == expected and page["strips"][0]["compression"] == 24
    assert summary(decode(fixture))["jobs"][0]["pages"][0]["black_pixels"] == 14
    invalid = (b"", b"\x00", b"\x18", b"\xb8\xfe", b"\xb8\xff\x00",
               b"\xb8\xff\x00\x00", b"\xb8\x06", b"\xb8\x04", b"\xb9\x05",
               b"\x20", b"\x40", b"\x60", b"\x80", b"\xa0",
               bytes.fromhex("07aafefb") + b"\xaa" * 512,
               bytes.fromhex("07aa00") + b"\xaa" * 6,
               bytes.fromhex("b80500"), bytes.fromhex("1f02aa00") + b"\xaa" * 7)
    for broken in invalid:
        try:
            delta_rows(broken, 8, 1)
        except ProtocolError:
            pass
        else:
            raise AssertionError(f"Malformed/unsupported DeltaPlus accepted: {broken[:8].hex()}")
    try:
        delta_rows(bytes.fromhex("07aafefb") + b"\xaa" * 512, 888, 1)
    except ProtocolError as exc:
        assert "512-byte limit" in str(exc)
    else:
        raise AssertionError("513-byte literal accepted within a wider row")
    for length in range(len(payload)):
        try:
            decode(capture(payload[:length]))
        except ProtocolError:
            pass
        else:
            raise AssertionError(f"Truncated DeltaPlus accepted: {length}")
    try:
        decode(capture(b"", MAX_STRIP_BYTES + 1))
    except ProtocolError as exc:
        assert "oversized" in str(exc)  # Checked before trying to take payload.
    else:
        raise AssertionError("Oversized DeltaPlus accepted")
    original_limit = MAX_DECODED_BYTES
    try:
        MAX_DECODED_BYTES = 40
        assert decode(fixture)[0]["pages"][0]["rows"] == expected  # 24 decoded bytes.
        try:
            decode(fixture + fixture)  # Small wire, 48 bytes across two jobs.
        except ProtocolError as exc:
            assert "Decoded raster" in str(exc)
        else:
            raise AssertionError("Aggregate decoded-byte limit not enforced")
    finally:
        MAX_DECODED_BYTES = original_limit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--output-dir", "--pbm-dir", type=Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("Independent RAW/DeltaPlus fixtures and corruption checks passed")
    if not args.capture:
        if not args.self_test:
            parser.error("capture or --self-test required")
        return
    try:
        if args.capture.stat().st_size > MAX_BYTES:
            raise ProtocolError("Capture exceeds 128 MiB limit")
        with args.capture.open("rb") as stream:
            data = stream.read(MAX_BYTES + 1)
        jobs = decode(data)
    except (OSError, ProtocolError) as exc:
        parser.exit(1, f"Invalid capture: {exc}\n")
    if args.output_dir:
        args.output_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        for j, job in enumerate(jobs, 1):
            for p, page in enumerate(job["pages"], 1):
                path = args.output_dir / f"job-{j:03d}-page-{p:03d}.pbm"
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "wb") as out:
                    out.write(f"P4\n{page['width']} {page['height']}\n".encode("ascii"))
                    for row in page["rows"]:
                        out.write(row)
    print(json.dumps(summary(jobs), indent=2))


if __name__ == "__main__":
    main()
