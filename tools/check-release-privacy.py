#!/usr/bin/env python3
"""Fail release packaging on personal build metadata or recognizable secrets."""
import argparse
from pathlib import Path
import re
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PATTERNS = {
    "personal home path": rb"/(?:Users|home)/[A-Za-z0-9_.-]+/",
    "macOS temporary path": rb"/(?:private/)?var/folders/[A-Za-z0-9_/.-]+",
    "local email address": rb"[A-Za-z0-9_.+-]+@[A-Za-z0-9_.-]+\.(?:lan|local)\b",
    "private key": rb"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----",
    "GitHub token": rb"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,})",
    "AWS access key": rb"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b",
    "Slack token": rb"xox[baprs]-[A-Za-z0-9-]{15,}",
    "API key": rb"\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{35,}",
    "credential URL": rb"https?://[^\s/:]+:[^\s/@]+@",
}


def public_files():
    paths = (ROOT / "scripts/public-files.txt").read_text().splitlines()
    if len(paths) != len(set(paths)):
        raise ValueError("Duplicate public manifest entry")
    for name in paths:
        path = Path(name)
        if (path.is_absolute() or ".." in path.parts or not path.parts or
                (any(p.startswith('.') for p in path.parts) and name != '.gitignore')):
            raise ValueError("Invalid public manifest entry")
        full = ROOT / path
        if any(p.is_symlink() for p in [full, *full.parents]) or not full.is_file():
            raise ValueError(f"Public file missing or not a regular file: {name}")
        yield name, full.read_bytes()


def inspect(name, data):
    # Only the exact project's copyright line is an approved personal identifier.
    copyright = re.search(rb"^Copyright \(c\) \d{4} (.+)$", (ROOT / "LICENSE").read_bytes(), re.M)
    if Path(name).name in ("LICENSE", "HP1010-MIT-LICENSE.txt") and copyright:
        data = data.replace(copyright[0], b"")
    patterns = dict(PATTERNS)
    if copyright:
        patterns["author identity outside license"] = re.escape(copyright[1])
        patterns["author username outside license"] = re.escape(re.sub(rb"\s+", b"", copyright[1]))
    return [f"{name}: {label}" for label, pattern in patterns.items() if re.search(pattern, data, re.I)]


def check(entries):
    findings = []
    count = 0
    for name, data in entries:
        count += 1
        if any(p in (".git", ".cache", ".DS_Store", "__MACOSX") for p in Path(name).parts):
            findings.append(f"{name}: private packaging metadata")
        findings.extend(inspect(name, name.encode() + b"\0" + data))
    if findings:
        raise ValueError("Release privacy check failed:\n" + "\n".join(findings))
    print(f"PASS release privacy scan: {count} files; license attribution allowed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path, help="App directory or ZIP; omit to scan the public source manifest")
    args = parser.parse_args()
    if args.path is None:
        check(public_files())
    elif args.path.is_dir():
        entries = []
        for path in sorted(args.path.rglob('*')):
            if path.is_symlink():
                raise ValueError("Release must not contain symbolic links")
            if path.is_file():
                entries.append((str(path.relative_to(args.path)), path.read_bytes()))
        check(entries)
    else:
        with zipfile.ZipFile(args.path) as archive:
            check((name, archive.read(name)) for name in archive.namelist() if not name.endswith('/'))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        sys.exit(str(error))
