#!/usr/bin/env python3
"""Exercise release safeguards offline; never invoke GitHub or build an app."""
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import release

SHA = 'a' * 40


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.app = self.root / release.APP_NAME
        (self.root / "config").mkdir()
        shutil.copyfile(release.ROOT / "config/dependencies.json", self.root / "config/dependencies.json")

    def bundle(self):
        for name in (*release.EXECUTABLES, *release.RESOURCES):
            path = self.app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'resource')
        for name in release.EXECUTABLES:
            (self.app / name).chmod(0o755)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps({
            'CFBundleVersion': '0.1.0', 'CFBundleShortVersionString': '0.1.0',
            'CFBundleExecutable': 'LaserJet1010Helper',
        }))

        for bundled, source in release.notice_sources(self.root).items():
            path = self.root / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((self.app / bundled).read_bytes())
        dependency = release.dependency_path('openssl', self.root)
        version = release.read_dependencies(self.root)['openssl']['version']
        (dependency / 'include/openssl').mkdir(parents=True, exist_ok=True)
        (dependency / 'include/openssl/opensslv.h').write_text(f'# define OPENSSL_VERSION_STR "{version}"\n')
        (dependency / 'LICENSE.txt').write_bytes(b'resource')
        for path in (self.root / 'THIRD_PARTY_NOTICES.md', self.app / 'Contents/Resources/THIRD_PARTY_NOTICES.md'):
            path.write_text(f'## OpenSSL {version}\n')

    def test_single_argument_failure_preserves_original_diagnostic(self):
        with self.assertRaisesRegex(ValueError, r'Command failed \(1\): /usr/bin/false'):
            release.run(['/usr/bin/false'])

    def test_stale_bundled_notices_and_licenses_are_rejected(self):
        self.bundle()
        release.validate_notices(self.root, self.app)
        for bundled in release.notice_sources(self.root):
            path = self.app / bundled
            original = path.read_bytes()
            path.write_bytes(b'old notice')
            with self.subTest(bundled=bundled), self.assertRaisesRegex(ValueError, 'differs from current source'):
                release.validate_notices(self.root, self.app)
            path.write_bytes(original)

    def test_notices_headers_and_license_must_match_dependency_pin(self):
        self.bundle()
        for path in (self.root / 'THIRD_PARTY_NOTICES.md', self.app / 'Contents/Resources/THIRD_PARTY_NOTICES.md'):
            path.write_text('## OpenSSL 3.6.0\n')
        with self.assertRaisesRegex(ValueError, 'notices do not match'):
            release.validate_notices(self.root, self.app)
        self.bundle()
        dependency = release.dependency_path('openssl', self.root)
        (dependency / 'include/openssl/opensslv.h').write_text('# define OPENSSL_VERSION_STR "3.6.0"\n')
        with self.assertRaisesRegex(ValueError, 'headers do not match'):
            release.validate_notices(self.root, self.app)
        self.bundle()
        (dependency / 'LICENSE.txt').write_bytes(b'new license')
        with self.assertRaisesRegex(ValueError, 'verified dependency source'):
            release.validate_notices(self.root, self.app)

    def preflight_runner(self, args, cwd=None, **kwargs):
        if 'status' in args and args[0] == 'git':
            return ''
        if 'rev-parse' in args:
            return SHA
        if any('/git/commits/' in str(arg) for arg in args):
            return SHA
        return ''

    def test_version_is_safe_for_bundle_tag_and_filename(self):
        for version in ('0.1.0', '1.0.0', '9999.99.99'):
            (self.root / 'VERSION').write_text(version + '\n')
            self.assertEqual(release.read_version(self.root), version)
        for version in ('', 'v1.0.0', '1.2', '01.2.3', '1.2.3-beta', '10000.0.0',
                        '1.100.0', '1.0.100', '../1.2.3', '1.2.3\n2.3.4'):
            (self.root / 'VERSION').write_text(version)
            with self.subTest(version=version), self.assertRaises(ValueError):
                release.read_version(self.root)

    def test_dirty_source_prevents_any_remote_call(self):
        with patch.object(release, 'run', return_value=' M VERSION') as run:
            with self.assertRaisesRegex(ValueError, 'clean working tree'):
                release.publish(self.root, '0.1.0', self.root / 'out.zip')
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[0][0], 'git')

    def test_existing_local_tag_remote_tag_or_release_fails_closed(self):
        for existing in ('local', 'remote', 'release'):
            def command(args, cwd=None, **kwargs):
                if existing == 'local' and args[:2] == ['git', 'tag']:
                    return 'v0.1.0'
                if existing == 'remote' and 'ls-remote' in args:
                    return SHA + '\trefs/tags/v0.1.0'
                if existing == 'release' and '--paginate' in args:
                    return 'v0.1.0'
                return self.preflight_runner(args)
            with self.subTest(existing=existing), patch.object(release, 'run', side_effect=command) as run:
                with self.assertRaisesRegex(ValueError, 'already exists'):
                    release.publish_preflight(self.root, 'example/project', 'v0.1.0', SHA)
                self.assertFalse(any('--method' in call.args[0] for call in run.call_args_list))

    def test_network_or_auth_failure_is_not_treated_as_missing_release(self):
        with patch.object(release, 'run', side_effect=ValueError('authentication failed')):
            with self.assertRaisesRegex(ValueError, 'authentication failed'):
                release.publish_preflight(self.root, 'example/project', 'v0.1.0', SHA)

    def test_commit_must_exist_remotely(self):
        with patch.object(release, 'run', return_value=''):
            with self.assertRaisesRegex(ValueError, 'push it'):
                release.publish_preflight(self.root, 'example/project', 'v0.1.0', SHA)

    def test_origin_is_explicit_github_repository(self):
        for origin in ('https://github.com/example/project.git', 'git@github.com:example/project.git',
                       'ssh://git@github.com/example/project.git'):
            with patch.object(release, 'run', return_value=origin):
                self.assertEqual(release.github_repository(self.root), 'example/project')
        with patch.object(release, 'run', return_value='https://example.com/project'):
            with self.assertRaisesRegex(ValueError, 'origin remote'):
                release.github_repository(self.root)

    def test_bundle_rejects_wrong_version_missing_resource_and_external_dependency(self):
        self.bundle()
        with patch.object(release, 'run', return_value='binary:\n\t/usr/lib/libSystem.B.dylib (version)'):
            release.validate_bundle(self.root, self.app, '0.1.0')
            with self.assertRaisesRegex(ValueError, 'version does not match'):
                release.validate_bundle(self.root, self.app, '0.2.0')
        with patch.object(release, 'run', return_value='binary:\n\t/opt/homebrew/lib/libssl.dylib (version)'):
            with self.assertRaisesRegex(ValueError, 'Non-system runtime dependency'):
                release.validate_bundle(self.root, self.app, '0.1.0')
        (self.app / release.RESOURCES[-1]).unlink()
        with self.assertRaisesRegex(ValueError, 'missing required content'):
            release.validate_bundle(self.root, self.app, '0.1.0')

    def test_zip_contains_all_bytes_and_executable_permissions(self):
        self.bundle()
        archive = self.root / 'test.zip'
        with zipfile.ZipFile(archive, 'w') as zipped:
            for path in self.app.rglob('*'):
                if path.is_file():
                    zipped.write(path, f'{release.APP_NAME}/{path.relative_to(self.app)}')
        release.validate_archive(archive, self.app)
        (self.app / release.EXECUTABLES[0]).chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'permissions differ'):
            release.validate_archive(archive, self.app)
        (self.app / release.EXECUTABLES[0]).chmod(0o755)
        (self.app / release.RESOURCES[1]).write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'content differs'):
            release.validate_archive(archive, self.app)

    def test_zip_rejects_incorrect_bundle_file_set(self):
        self.bundle()
        archive = self.root / 'test.zip'
        with zipfile.ZipFile(archive, 'w') as zipped:
            zipped.writestr('unexpected', b'file')
        with self.assertRaisesRegex(ValueError, 'exactly the complete app'):
            release.validate_archive(archive, self.app)

    def test_publish_uses_exact_snapshot_and_only_app_asset(self):
        destination = self.root / 'build/LaserJet1010Helper-v0.1.0.zip'
        calls = []
        def command(args, cwd=None, **kwargs):
            calls.append(args)
            if args[:2] == ['git', 'clone']:
                Path(args[-1]).mkdir()
            if any('/git/ref/tags/' in str(arg) for arg in args):
                return SHA
            return ''
        def package(root, version, output):
            self.assertNotEqual(root, self.root)
            self.assertFalse((root / '.cache').exists())
            output.parent.mkdir(parents=True)
            output.write_bytes(b'validated app archive')
        with patch.object(release, 'run', side_effect=command), \
             patch.object(release, 'clean_commit', return_value=SHA), \
             patch.object(release, 'github_repository', return_value='example/project'), \
             patch.object(release, 'publish_preflight') as preflight, \
             patch.object(release, 'read_version', return_value='0.1.0'), \
             patch.object(release, 'package', side_effect=package):
            release.publish(self.root, '0.1.0', destination)
        self.assertEqual(preflight.call_count, 2)
        clone = next(args for args in calls if args[:2] == ['git', 'clone'])
        self.assertIn('--no-local', clone)
        self.assertIn(['git', 'checkout', '--quiet', '--detach', SHA], calls)
        create_tag = next(args for args in calls if '--method' in args)
        self.assertIn('sha=' + SHA, create_tag)
        self.assertIn('ref=refs/tags/v0.1.0', create_tag)
        create_release = next(args for args in calls if args[:3] == ['gh', 'release', 'create'])
        self.assertIn('--verify-tag', create_release)
        self.assertEqual(create_release[create_release.index('--target') + 1], SHA)
        self.assertEqual([Path(arg).name for arg in create_release if str(arg).endswith('.zip')], [destination.name])
        self.assertEqual(destination.read_bytes(), b'validated app archive')
        self.assertFalse(any('--force' in args or '--clobber' in args for args in calls))

    def test_missing_prerequisites_fail_clearly(self):
        with patch.object(release.sys, 'platform', 'darwin'), \
             patch.object(release.shutil, 'which', side_effect=lambda name: None if name == 'gh' else '/bin/tool'):
            release.prerequisites(False)
            with self.assertRaisesRegex(ValueError, 'Missing release prerequisites: gh'):
                release.prerequisites(True)

    def test_version_change_during_build_preserves_existing_archive(self):
        destination = self.root / 'out.zip'
        destination.write_bytes(b'previous archive')
        with patch.object(release, 'run') as run, \
             patch.object(release, 'read_version', return_value='0.2.0'):
            with self.assertRaisesRegex(ValueError, 'VERSION changed'):
                release.package(self.root, '0.1.0', destination)
            self.assertEqual(run.call_count, 1)
        self.assertEqual(destination.read_bytes(), b'previous archive')

    def test_atomic_tag_creation_race_cannot_create_or_upload_release(self):
        def command(args, cwd=None, **kwargs):
            if '--method' in args:
                raise ValueError('Reference already exists')
            return ''
        def package(root, version, output):
            output.parent.mkdir(parents=True)
            output.write_bytes(b'app')
        with patch.object(release, 'run', side_effect=command) as run, \
             patch.object(release, 'clean_commit', return_value=SHA), \
             patch.object(release, 'github_repository', return_value='example/project'), \
             patch.object(release, 'publish_preflight'), \
             patch.object(release, 'read_version', return_value='0.1.0'), \
             patch.object(release, 'package', side_effect=package):
            with self.assertRaisesRegex(ValueError, 'Reference already exists'):
                release.publish(self.root, '0.1.0', self.root / 'out.zip')
            self.assertFalse(any(call.args[0][:2] == ['gh', 'release'] for call in run.call_args_list))

    def test_source_change_during_snapshot_build_prevents_tag_creation(self):
        def package(root, version, output):
            output.parent.mkdir(parents=True)
            output.write_bytes(b'app')
        for changed in ([SHA, SHA, 'b' * 40], [SHA, SHA, SHA, 'b' * 40]):
            with patch.object(release, 'run') as run, \
                 patch.object(release, 'clean_commit', side_effect=changed), \
                 patch.object(release, 'github_repository', return_value='example/project'), \
                 patch.object(release, 'publish_preflight'), \
                 patch.object(release, 'read_version', return_value='0.1.0'), \
                 patch.object(release, 'package', side_effect=package):
                with self.assertRaisesRegex(ValueError, 'Source changed'):
                    release.publish(self.root, '0.1.0', self.root / 'out.zip')
                self.assertFalse(any('--method' in call.args[0] for call in run.call_args_list))


if __name__ == '__main__':
    unittest.main()
