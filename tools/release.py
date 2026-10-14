#!/usr/bin/env python3
"""Build a local app archive, or publish a clean, exact-commit GitHub release."""
import argparse
import os
from pathlib import Path
import plistlib
import re
import shutil
import shlex
import subprocess
import sys
import tempfile
import zipfile

from project_version import read_version
from dependencies import dependency_path, read_dependencies

ROOT = Path(__file__).resolve().parents[1]
APP_NAME = 'LaserJet 1010 Helper.app'
EXECUTABLES = ('Contents/MacOS/LaserJet1010Helper', 'Contents/Helpers/hp1010-helper')
RESOURCES = (
    'Contents/Info.plist', 'Contents/Resources/AppIcon.icns',
    'Contents/Resources/THIRD_PARTY_NOTICES.md',
    'Contents/Resources/Licences/HP1010-MIT-LICENSE.txt',
    'Contents/Resources/Licences/HP-REFERENCE-LICENSE.txt',
    'Contents/Resources/Licences/LICENSE', 'Contents/Resources/Licences/NOTICE',
    'Contents/Resources/Licences/OpenSSL-LICENSE.txt',
)


NOTICE_SOURCES = {
    'Contents/Resources/THIRD_PARTY_NOTICES.md': 'THIRD_PARTY_NOTICES.md',
    'Contents/Resources/Licences/HP1010-MIT-LICENSE.txt': 'LICENSE',
    'Contents/Resources/Licences/HP-REFERENCE-LICENSE.txt': 'licences/HP-REFERENCE-LICENSE.txt',
    'Contents/Resources/Licences/OpenSSL-LICENSE.txt': 'licences/OpenSSL-LICENSE.txt',
}


def notice_sources(root):
    sources = dict(NOTICE_SOURCES)
    for name in ("LICENSE", "NOTICE"):
        sources[f"Contents/Resources/Licences/{name}"] = dependency_path("pappl", root) / name
    return sources


def validate_notices(root, app):
    for bundled, source in notice_sources(root).items():
        if (app / bundled).read_bytes() != (root / source).read_bytes():
            raise ValueError(f'Bundled notice/license differs from current source: {bundled}')
    version = read_dependencies(root)['openssl']['version']
    dependency = dependency_path('openssl', root)
    header = (dependency / 'include/openssl/opensslv.h').read_text()
    actual = re.search(r'^#\s*define\s+OPENSSL_VERSION_STR\s+"([^"]+)"', header, re.M)
    if not actual or actual[1] != version:
        raise ValueError('Built OpenSSL headers do not match the pinned version.')
    notices = (root / 'THIRD_PARTY_NOTICES.md').read_text()
    versions = re.findall(r'\bOpenSSL (\d+\.\d+\.\d+)\b', notices)
    if not versions or set(versions) != {version}:
        raise ValueError('Third-party notices do not match the pinned OpenSSL version.')
    if (dependency / 'LICENSE.txt').read_bytes() != (root / 'licences/OpenSSL-LICENSE.txt').read_bytes():
        raise ValueError('Checked-in OpenSSL license differs from the verified dependency source.')


def run(args, cwd=ROOT, stream=False):
    result = subprocess.run([str(arg) for arg in args], cwd=cwd, text=True,
                            capture_output=not stream)
    if result.returncode:
        detail = (result.stderr or result.stdout or '').strip()
        raise ValueError(f"Command failed ({result.returncode}): {shlex.join([str(arg) for arg in args])}\n{detail}")
    return (result.stdout or '').strip()


def prerequisites(publish):
    if sys.platform != 'darwin':
        raise ValueError('Releases must be built on macOS (Apple Silicon, macOS 26 SDK).')
    required = ['git', 'swiftc', 'swift', 'cc', 'make', 'perl', 'curl', 'shasum',
                'tar', 'patch', 'strip', 'iconutil', 'codesign', 'xattr', 'ditto', 'otool']
    if publish:
        required.append('gh')
    missing = [name for name in required if shutil.which(name) is None]
    if missing:
        raise ValueError('Missing release prerequisites: ' + ', '.join(missing))


def clean_commit(root):
    if run(['git', 'status', '--porcelain', '--untracked-files=all'], root):
        raise ValueError('Publishing requires a clean working tree; commit all source changes first.')
    return run(['git', 'rev-parse', '--verify', 'HEAD^{commit}'], root)


def github_repository(root):
    remote = run(['git', 'remote', 'get-url', 'origin'], root)
    match = re.fullmatch(r'(?:https://github\.com/|git@github\.com:|ssh://git@github\.com/)([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(?:\.git)?/?', remote)
    if not match:
        raise ValueError('Publishing requires an origin remote on github.com (HTTPS or SSH).')
    return match[1]


def publish_preflight(root, repo, tag, commit):
    run(['gh', 'auth', 'status', '--hostname', 'github.com'], root)
    if run(['git', 'tag', '--list', tag], root):
        raise ValueError(f'Tag {tag} already exists locally; choose a new VERSION. Tags are never replaced.')
    if run(['git', 'ls-remote', '--tags', 'origin', f'refs/tags/{tag}', f'refs/tags/{tag}^{{}}'], root):
        raise ValueError(f'Tag {tag} already exists remotely; choose a new VERSION. Tags are never replaced.')
    tags = run(['gh', 'api', '--hostname', 'github.com', f'repos/{repo}/releases', '--paginate', '--jq', '.[].tag_name'], root)
    if tag in tags.splitlines():
        raise ValueError(f'Release {tag} already exists; existing releases and assets are never replaced.')
    try:
        remote_commit = run(['gh', 'api', '--hostname', 'github.com', f'repos/{repo}/git/commits/{commit}', '--jq', '.sha'], root)
    except ValueError as error:
        raise ValueError('Cannot verify source commit on GitHub; push the commit first and check repository access.\n' + str(error)) from error
    if remote_commit != commit:
        raise ValueError('The exact source commit must already be available on GitHub; push it before publishing.')


def validate_bundle(root, app, version):
    for relative in (*EXECUTABLES, *RESOURCES):
        path = app / relative
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f'App is missing required content: {relative}')
    with (app / 'Contents/Info.plist').open('rb') as source:
        info = plistlib.load(source)
    if any(info.get(key) != version for key in ('CFBundleVersion', 'CFBundleShortVersionString')):
        raise ValueError('App version does not match VERSION.')
    if info.get('CFBundleExecutable') != 'LaserJet1010Helper':
        raise ValueError('App executable does not match the bundle layout.')
    validate_notices(root, app)
    for relative in EXECUTABLES:
        executable = app / relative
        if not os.access(executable, os.X_OK):
            raise ValueError(f'App executable is not executable: {relative}')
        # PAPPL and OpenSSL are statically linked. Runtime libraries must come
        # from macOS, never Homebrew or another machine-local installation.
        dependencies = run(['otool', '-L', executable], root).splitlines()[1:]
        if not dependencies:
            raise ValueError(f'Cannot inspect executable dependencies: {relative}')
        for line in dependencies:
            dependency = line.strip().split(' (', 1)[0]
            if not dependency.startswith(('/usr/lib/', '/System/Library/')):
                raise ValueError(f'Non-system runtime dependency in {relative}: {dependency}')
    run(['codesign', '--verify', '--deep', '--strict', app], root)
    run([sys.executable, root / 'tools/check-release-privacy.py', app], root, stream=True)


def validate_archive(archive, app):
    expected = {f'{APP_NAME}/{p.relative_to(app).as_posix()}': p
                for p in app.rglob('*') if p.is_file()}
    with zipfile.ZipFile(archive) as bundle:
        entries = [entry for entry in bundle.infolist() if not entry.is_dir()]
        if len(entries) != len(expected) or {entry.filename for entry in entries} != set(expected):
            raise ValueError('ZIP does not contain exactly the complete app bundle.')
        for entry in entries:
            path = expected[entry.filename]
            if bundle.read(entry) != path.read_bytes():
                raise ValueError(f'ZIP content differs from app: {entry.filename}')
            if (entry.external_attr >> 16) & 0o777 != path.stat().st_mode & 0o777:
                raise ValueError(f'ZIP permissions differ from app: {entry.filename}')


def package(root, version, destination):
    run([root / 'scripts/build-app.sh'], root, stream=True)
    if read_version(root) != version:
        raise ValueError('VERSION changed during the build; retry with stable source.')
    # Finder/File Provider may attach metadata to the build folder after the
    # build finishes. Package a private copy with that metadata stripped.
    with tempfile.TemporaryDirectory(prefix='hp1010-package-', dir='/tmp') as stage:
        app = Path(stage) / APP_NAME
        run(['ditto', '--norsrc', '--noextattr', root / 'build' / APP_NAME, app], root)
        run(['xattr', '-c', app], root)
        validate_bundle(root, app, version)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.release-', dir=destination.parent) as temporary:
            archive = Path(temporary) / destination.name
            run(['ditto', '--norsrc', '--noextattr', '-c', '-k', '--keepParent', app, archive], root)
            validate_archive(archive, app)
            run([sys.executable, root / 'tools/check-release-privacy.py', archive], root, stream=True)
            archive.replace(destination)
    print(f'Packaged {destination.name}', flush=True)


def publish(root, version, destination):
    commit = clean_commit(root)
    repo = github_repository(root)
    tag = f'v{version}'
    publish_preflight(root, repo, tag, commit)
    with tempfile.TemporaryDirectory(prefix='hp1010-release-', dir='/tmp') as temporary:
        source = Path(temporary) / 'source'
        # A detached clone preserves tracked files even when .gitattributes uses
        # export-ignore/export-subst. No ignored build or dependency cache is copied.
        run(['git', 'clone', '--quiet', '--no-local', '--no-checkout', '--', root, source], root)
        run(['git', 'checkout', '--quiet', '--detach', commit], source)
        if clean_commit(source) != commit or read_version(source) != version:
            raise ValueError('Release snapshot does not match the selected source commit/version.')
        snapshot_archive = source / 'build' / destination.name
        package(source, version, snapshot_archive)
        if clean_commit(source) != commit or clean_commit(root) != commit:
            raise ValueError('Source changed during the release build; refusing to publish.')
        # Repeat checks after the potentially long dependency build.
        publish_preflight(root, repo, tag, commit)
        # Keep a local copy, but upload the validated snapshot archive directly.
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.release-', dir=destination.parent) as staging:
            copy = Path(staging) / destination.name
            shutil.copyfile(snapshot_archive, copy)
            copy.replace(destination)
        # GitHub ref creation is atomic and fails if the tag already exists.
        run(['gh', 'api', '--hostname', 'github.com', '--method', 'POST', f'repos/{repo}/git/refs',
             '-f', f'ref=refs/tags/{tag}', '-f', f'sha={commit}'], root)
        try:
            actual = run(['gh', 'api', '--hostname', 'github.com', f'repos/{repo}/git/ref/tags/{tag}', '--jq', '.object.sha'], root)
            if actual != commit:
                raise ValueError('Remote tag does not identify the exact build commit.')
            notes = (f'Download {destination.name} and unzip it to obtain {APP_NAME}.\n\n'
                     f'Built from commit {commit}. Matching source: https://github.com/{repo}/tree/{tag}\n'
                     'Source archives are provided automatically by GitHub.\n\n'
                     'Apple Silicon; macOS 26 or later. Ad-hoc signed; not notarized.')
            run(['gh', 'release', 'create', tag, snapshot_archive, '--repo', f'github.com/{repo}',
                 '--verify-tag', '--target', commit, '--title', f'LaserJet 1010 Helper {version}',
                 '--notes', notes], root, stream=True)
        except ValueError as error:
            raise ValueError(f'{error}\nTag {tag} was created and is preserved. Inspect GitHub before recovery; this command will not replace tags, releases, or assets.') from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--package-only', action='store_true', help='Build/package local source; permits uncommitted changes, makes no remote changes.')
    mode.add_argument('--publish', action='store_true', help='Build a clean committed snapshot and publish a new GitHub tag and release.')
    args = parser.parse_args()
    version = read_version(ROOT)
    prerequisites(args.publish)
    destination = ROOT / 'build' / f'LaserJet1010Helper-v{version}.zip'
    if args.publish:
        publish(ROOT, version, destination)
    else:
        package(ROOT, version, destination)
        print('Local package only; this archive is not a published release.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        sys.exit(f'Release failed: {error}')
