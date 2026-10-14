# Testing

Run the complete software validation on Apple Silicon macOS from the repository root:

```sh
scripts/test-all.sh
```

This builds the helper, runs every software suite, then builds and validates a local app ZIP. It does not install the app, change CUPS queues, publish a release, or open physical USB interfaces. It requires local loopback sockets and macOS icon/signature utilities; a restricted sandbox may need those permissions.

Individual checks remain available for development:

```sh
scripts/build-helper.sh
python3 tests/test_dependency_patches.py
SANITIZE=1 HP1010_SKIP_USB_PROBE=1 scripts/test-core.sh
python3 tests/integration.py
python3 tests/regression.py
scripts/test-shutdown.sh
scripts/test-drain.sh
scripts/test-installation.sh
scripts/test-printer-setup.sh
scripts/test-printing-service.sh
scripts/test-security.sh
python3 tests/multipart.py
python3 tests/test_release_privacy.py
python3 tests/test_release.py
python3 tools/check-release-privacy.py
```

The dependency tests apply both patches to the verified upstream archive, reject changed upstream files before editing anything, and reject repeated application. To update PAPPL, review and update the unified patches and their `manifest.json` before changing the dependency pin. Input/raster adapter changes and shared protocol limits are part of the build fingerprint.

The printing-service tests inject process, time, storage, and printer setup operations. They cover readiness and startup failures, explicit retry/repair, stale callbacks after restart, forced shutdown, quit completion, and malformed/stale status files. They never launch the real app or configure a printer.

The core suite checks the FastRaster encoder, transport behavior, sanitizer safety, and the independent decoder. The IPP integration and regression suites exercise the loopback service, raster reconstruction, bounds, malformed input, failures, cancellation, privacy, and cleanup. The shutdown and drain harnesses check bounded process and job shutdown without requiring a printer.

The installation tests use temporary app bundles and injected copy, verification, and launch operations. Printer setup tests cover existing and missing queues, repair, failure, cancellation, output limits, and subprocess deadlines without changing system printers.

The multipart regression compiles the patched PAPPL form parser with AddressSanitizer and UndefinedBehaviorSanitizer and exposes it only on a temporary test server. It checks valid forms, the upstream missing-delimiter trigger, short/empty boundaries, oversized bodies, and recovery. Production web forms remain disabled.

The security suite checks the exact Host/port allowlist, HTTP/1.0 and HTTP/1.1 missing-Host rejection, browser/form/preflight rejection, unauthorized job access and mutations, spoofed usernames, and authentication isolation between keep-alive requests. It exercises macOS libcups' real authentication challenge/retry, Create-Job and Send-Document and compares the resulting raster output. Sanitized native tests check credential persistence, permissions, ACLs, symlinks, hard links, FIFOs, corrupt files, and CUPS request construction. The CUPS connection is replaced in the native queue-write tests: the installed system scheduler, its access policy, and the root-only IPP backend are not exercised. Printer-setup mocks verify credential installation, migration, failure, and preservation of paused queues. Native tests also verify that repeated setup does not write the device URI or restart a job, and that migration fails closed when the queue is busy or its job count cannot be read.

The app build strips debug metadata before signing and scans the staged app for personal paths, identifiers, and recognizable credentials. The separate release command additionally scans the packaged ZIP. The source exporter scans its explicit public file manifest. These pattern checks supplement manual review; they do not guarantee detection of every possible secret. Review dependency advisories and update the pinned sources and hashes before future releases. Build fingerprints invalidate cached libraries after build instructions change.

For the build/release boundary, run `scripts/build-app.sh` and confirm it produces `build/LaserJet 1010 Helper.app` without creating or modifying any distribution ZIP. Existing ZIPs may remain from earlier packaging runs; compare their hashes or modification times before and after the build.

Run `scripts/release.sh --package-only` to build a local release preview without remote changes. Inspect `build/LaserJet1010Helper-v<VERSION>.zip` with `unzip -l`, then extract it to a temporary directory with `ditto -x -k`. Verify:

- There is one top-level `LaserJet 1010 Helper.app` containing `Contents/MacOS/LaserJet1010Helper`, `Contents/Helpers/hp1010-helper`, `Contents/Info.plist`, the icon, notices, and licenses.
- Bundled notices and licenses match the current source and verified dependencies; the OpenSSL notice and headers match the build pin.
- Both `CFBundleShortVersionString` and `CFBundleVersion` in the extracted `Info.plist` match `VERSION` and the archive filename.
- `codesign --verify --deep --strict` succeeds on the extracted app, and both executables retain executable permissions.
- `otool -L` on both executables shows no external PAPPL, OpenSSL, Homebrew, or build-directory dependencies. PAPPL and OpenSSL are linked statically; macOS system libraries remain expected.
- The extracted app opens and its helper starts. A local preview may include uncommitted changes; only publishing enforces the release's exact-commit provenance.

The release regression suite (`python3 tests/test_release.py`) uses local fixtures and mocked commands to check package validation and publishing safeguards without contacting or modifying GitHub. Do not invoke publishing against a live remote just to test it.

After `scripts/build-app.sh`, check the app’s **Move to Applications** and replacement prompts, then verify the installed copy opens without another install prompt. With no pending print jobs, check automatic registration of a missing queue, credential installation on an existing queue without changing its paused state or printing preferences, **Retry Setup** after a failure, and **Repair Printer Setup** from the menu. Check Start/Stop and Quit cleanup.

For a manual simulated IPP transfer, build the test tools with `scripts/test-security.sh`, then start the helper in a private temporary state directory:

```sh
state="$(mktemp -d)"
chmod 700 "$state"
build/hp1010-helper --simulate --state-dir "$state" --port 8631
```

In another terminal, generate a small raster with `build/tests/make-raster /tmp/test-page.pwg 128 129 1 1 pwg sparse`. Run `build/tests/test-auth-client 8631 STATE_DIRECTORY /tmp/test-page.pwg`, replacing `STATE_DIRECTORY` with the first terminal's temporary directory. The test client reads its credential directly from the private file and exercises CUPS authentication without exposing the credential in command arguments. Confirm a `.fr` capture appears under the state's `captures` directory and decode it with `python3 tools/decode_fastraster.py PATH_TO_CAPTURE`. Stop the helper with Control-C and remove the temporary state and fixture. The test client reports failures with assertions and is not shipped in the application bundle.

Native Preview/browser print-dialog checks must additionally validate the installed app's authenticated CUPS queue on a test Mac. Do not configure a credential-free simulation queue: it can discover capabilities but cannot print. Neither the software suite nor the mock queue tests establish physical print success.

Hardware validation must cover A4 and Letter, portrait and landscape, multipage jobs, copies, cancellation, disconnect/reconnect, sleep/wake, and login launch. Confirm paper output directly before making hardware claims.
