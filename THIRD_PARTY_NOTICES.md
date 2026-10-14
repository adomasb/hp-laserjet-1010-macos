# Third-party notices

## PAPPL 1.4.12

The helper statically links PAPPL 1.4.12, Printer Application Library, by Michael R Sweet, Apple Inc., and Easy Software Products. Source: [v1.4.12 release](https://github.com/michaelrsweet/pappl/releases/tag/v1.4.12), archive [`pappl-1.4.12.tar.gz`](https://github.com/michaelrsweet/pappl/releases/download/v1.4.12/pappl-1.4.12.tar.gz), SHA-256 `1684c4e06446e9f7d93a39729fa0ba56f07a4007560080fdad7d0e2076a3615f`. `scripts/build-deps.sh` verifies this hash and uses `tools/patch_pappl.py` to apply the reviewed patches in `patches/pappl/`, with exact source hashes checked before and after patching. `config/dependencies.json` holds the build pins. The local input and raster adapters are compiled as separate C modules. PAPPL is Apache-2.0 with an optional GPLv2/LGPLv2 linking exception. Its upstream `LICENSE` and `NOTICE` are included in the app bundle under `Contents/Resources/Licences/`.

The local build also backports upstream multipart-parser fixes for [GHSA-r3w6-72jx-qfpx](https://github.com/michaelrsweet/pappl/security/advisories/GHSA-r3w6-72jx-qfpx) from commits [`aa566d9`](https://github.com/michaelrsweet/pappl/commit/aa566d9176801ac4247ad6412a9cb1953b58077d), [`1431b92`](https://github.com/michaelrsweet/pappl/commit/1431b92759a66d984a5326098cb5f1a04c58af6f), and [`d39fefd`](https://github.com/michaelrsweet/pappl/commit/d39fefd09ec527d6036d62fdb96fee2080ca856f). These changes retain PAPPL's license. The application keeps the web interface disabled. Local patches also enforce exact loopback Host/port validation, restrict the authenticated helper to IPP requests, authenticate every operation except printer capability discovery, and advertise its Basic authentication requirement. These integration changes are included in `patches/pappl/integration.patch`.

## OpenSSL 3.6.5

The helper statically links OpenSSL 3.6.5 (`libssl.a` and `libcrypto.a`), built locally by `scripts/build-openssl.sh` from the [official release archive](https://github.com/openssl/openssl/releases/download/openssl-3.6.5/openssl-3.6.5.tar.gz), SHA-256 `a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98`. OpenSSL 3 is licensed under Apache License 2.0. Its [`LICENSE.txt`](https://github.com/openssl/openssl/blob/openssl-3.6.5/LICENSE.txt) is included as [`licences/OpenSSL-LICENSE.txt`](licences/OpenSSL-LICENSE.txt) and copied from the verified source into the app bundle. No Homebrew installation is used or modified.

## HP FastRaster reference

The original encoder's protocol constants and reference facts were transcribed from HP's HPLIP 3.26.6 source. No HPLIP driver implementation or source archive is included. Reference: [HPLIP 3.26.6 archive](https://downloads.sourceforge.net/project/hplip/hplip/3.26.6/hplip-3.26.6.tar.gz), SHA-256 `5fb2235a9cb6ec07cb4786c4066b151359c301324d54aaabf69ae7eef35606b3`. The inspected `prnt/hpijs/ljfastraster.cpp` header identifies copyright 1996–2015 HP Co. and includes HP's three-condition BSD-style terms. Required attribution and license text are in [`licences/HP-REFERENCE-LICENSE.txt`](licences/HP-REFERENCE-LICENSE.txt), also bundled under `Contents/Resources/Licences/`.

HPLIP contains components under several licenses. Its model database has GPL-2.0-or-later terms and was consulted as research only; it is not included. HP LaserJet 1018/1020 protocol implementations are not used.

## System libraries and protocol references

CUPS, Apple IOKit, CoreFoundation, AppKit, ServiceManagement, zlib, and macOS system facilities are linked from the target macOS SDK or system installation; their versions depend on macOS and their source is not copied into the app. The project uses CUPS/PAPPL IPP handling and the USB-IF Printer Class Specification 1.1 as a protocol reference; the specification is not redistributed. References: [Apple CUPS administration manual](https://apple.github.io/cups/doc/admin.html) and [USB-IF specification](https://www.usb.org/sites/default/files/usbprint11a021811.pdf).

No old HP printer driver is packaged as this implementation. This documentation does not accept licenses on behalf of the user.
