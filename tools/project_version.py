#!/usr/bin/env python3
"""Read the shared application and release version from VERSION."""

from pathlib import Path
import re
import sys


def read_version(root: Path) -> str:
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    # Keep the same value valid for both macOS bundle version fields.
    if not re.fullmatch(r"(?:0|[1-9][0-9]{0,3})\.(?:0|[1-9][0-9]?)\.(?:0|[1-9][0-9]?)", version):
        raise ValueError(
            "VERSION must contain MAJOR.MINOR.PATCH with no leading zeroes "
            "and at most 4, 2, and 2 digits respectively"
        )
    return version


if __name__ == "__main__":
    try:
        print(read_version(Path(__file__).resolve().parents[1]))
    except (OSError, UnicodeError, ValueError) as error:
        sys.exit(str(error))
