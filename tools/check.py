#!/usr/bin/env python3
"""Offline publication checks; never opens device nodes or changes a desktop."""
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Checks(unittest.TestCase):
    def power_logic(self):
        module = load('power_daemon', ROOT / 'power-button/surface-screen-key.py')
        tree = ast.parse((ROOT / 'power-button/surface-screen-key.py').read_text())
        main = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'main')
        functions = [n for n in main.body if isinstance(n, ast.FunctionDef) and n.name in
                     ['value', 'finished', 'lock_changed', 'toggle', 'input_event']]
        calls, timeouts, requests = [], [], []
        properties = {'mode': 0, 'locked': False}
        class Proxy:
            def __init__(self, which):
                self.which = which
            def get_cached_property(self, name):
                value = properties[self.which]
                return SimpleNamespace(unpack=lambda: value)
        state = {'pending': False, 'busy': False, 'queued': False,
                 'requested_at': 0.0, 'last_press': -10.0, 'held': False}
        bus = SimpleNamespace(call=lambda *args: requests.append(args), call_finish=lambda _: None)
        namespace = {'state': state, 'time': SimpleNamespace(monotonic=lambda: 10.0),
            'display': Proxy('mode'), 'login': Proxy('locked'), 'system': bus,
            'GLib': SimpleNamespace(Error=RuntimeError, timeout_add=lambda _, f: timeouts.append(f),
                                    idle_add=lambda *args: None, IO_HUP=8, IO_ERR=16),
            'Gio': SimpleNamespace(DBusCallFlags=SimpleNamespace(NONE=0)),
            'candidates': [(0, 0, 0, 0, '/test/session')],
            'power': lambda mode: calls.append(mode), 'print': lambda *a, **kw: None,
            'os': module.os, 'EVENT': module.EVENT, 'fd': -1}
        exec(compile(ast.Module(body=functions, type_ignores=[]), '<isolated production logic>', 'exec'), namespace)
        return namespace, properties, calls, timeouts, requests

    def test_unconfirmed_lock_never_blanks(self):
        ns, properties, calls, timeouts, requests = self.power_logic()
        ns['toggle']()
        self.assertEqual(calls, [])
        self.assertTrue(ns['state']['pending'])
        self.assertEqual(len(requests), 1)
        ns['lock_changed']()
        self.assertEqual(calls, [])
        timeouts[0]()
        self.assertFalse(ns['state']['busy'])
        self.assertFalse(ns['state']['pending'])
        self.assertEqual(calls, [])

    def test_confirmed_lock_and_offscreen_wake(self):
        ns, properties, calls, _, _ = self.power_logic()
        ns['toggle']()
        properties['locked'] = True
        ns['lock_changed']()
        self.assertEqual(calls, [3])
        ns['state']['busy'] = False
        properties['mode'] = 3
        ns['toggle']()
        self.assertEqual(calls, [3, 0])
        self.assertTrue(properties['locked'])

    def test_raw_power_repeats_and_other_keys_are_ignored(self):
        ns, _, _, _, _ = self.power_logic()
        actions = []
        ns['toggle'] = lambda *args: actions.append('power')
        event = ns['EVENT']
        chunks = [event.pack(10, 0, 1, 115, 1), event.pack(10, 0, 1, 116, 1),
                  event.pack(10, 0, 1, 116, 2), event.pack(10, 0, 1, 116, 1),
                  event.pack(10, 0, 1, 116, 0), event.pack(10, 100000, 1, 116, 1)]
        with patch.object(ns['os'], 'read', side_effect=chunks + [BlockingIOError()]):
            ns['input_event'](None, 0)
        self.assertEqual(actions, ['power'])

    def test_power_install_restore_preserves_existing_files(self):
        module = load('power_install', ROOT / 'power-button/install.py')
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            path = root / 'etc/systemd/logind.conf.d/60-surface-screen-key.conf'
            path.parent.mkdir(parents=True)
            original = b'[Login]\nHandlePowerKey=suspend\n'
            path.write_bytes(original)
            module.apply(root, 'desktop')
            receipts = list(root.glob('var/lib/surface-screen-key/backups/*/receipt.json'))
            self.assertEqual(len(receipts), 1)
            self.assertEqual(path.read_text(), '[Login]\nHandlePowerKey=ignore\nIdleAction=ignore\n')
            module.restore(root, receipts[0])
            self.assertEqual(path.read_bytes(), original)
            self.assertFalse((root / 'usr/local/libexec/surface-screen-key.py').exists())
            self.assertTrue(receipts[0].exists())

    def test_power_restore_refuses_later_edits(self):
        module = load('power_install', ROOT / 'power-button/install.py')
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            module.apply(root, 'desktop')
            receipt = next(root.glob('var/lib/surface-screen-key/backups/*/receipt.json'))
            path = root / 'etc/systemd/logind.conf.d/60-surface-screen-key.conf'
            path.write_text('administrator edit\n')
            with self.assertRaises(ValueError):
                module.restore(root, receipt)
            self.assertEqual(path.read_text(), 'administrator edit\n')

    def test_grub_integrity_and_disabled_return_path(self):
        tool = load('grub_tool', ROOT / 'grub-buttons/toolbox/tool.py')
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            release = base / 'releases/d1-test'
            release.mkdir(parents=True)
            image = release / 'grub-daily.efi'
            image.write_bytes(b'isolated test artifact')
            manifest = {'kind': 'daily-D1', 'image': image.name, 'files': {image.name: {
                'sha256': tool.digest(image), 'bytes': image.stat().st_size}},
                'signed_image_sha256': tool.digest(image), 'theme': 'test',
                'menu_physical_acceptance': 'test only'}
            tool.atomic_json(release / 'manifest.json', manifest)
            tool.atomic_json(base / 'releases.json', {'releases': {'d1-test': {
                'manifest_sha256': tool.digest(release / 'manifest.json')}}})
            tool.set_daily(base, 'd1-test', False, 'test_install')
            self.assertNotIn('chainloader', (base / 'daily-active.cfg').read_text())
            self.assertTrue(tool.check_daily(base, system=False)['integrity_passed'])
            tool.set_daily(base, 'd1-test', True, 'test_enable')
            self.assertIn('chainloader', (base / 'daily-active.cfg').read_text())
            image.write_bytes(b'tampered')
            with self.assertRaises(ValueError):
                tool.validate_release(base, 'd1-test')

    def test_release_manifest_traversal_is_rejected(self):
        tool = load('grub_tool', ROOT / 'grub-buttons/toolbox/tool.py')
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            release = base / 'releases/d1-test'
            release.mkdir(parents=True)
            tool.atomic_json(release / 'manifest.json', {'files': {'../../outside': {}}})
            tool.atomic_json(base / 'releases.json', {'releases': {'d1-test': {
                'manifest_sha256': tool.digest(release / 'manifest.json')}}})
            with self.assertRaises(ValueError):
                tool.validate_release(base, 'd1-test')


def main():
    for file in ROOT.rglob('*.py'):
        if file.relative_to(ROOT).parts[0] in ['build', 'dist', '.git']:
            continue
        ast.parse(file.read_text(), filename=str(file))
    expected = (ROOT / 'vendor/SHA256SUMS').read_text().split()[0]
    if hashlib.sha256((ROOT / 'vendor/grub-2.12.tar.xz').read_bytes()).hexdigest() != expected:
        raise ValueError('Upstream archive changed')
    # Scan published text without printing any matched sensitive contents.
    patterns = [r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
                r'gh[pousr]_[A-Za-z0-9]{20,}', r'github_pat_[A-Za-z0-9_]{20,}',
                r'(?<![A-Za-z])sk-[A-Za-z0-9_-]{20,}', r'/root/surface-night-',
                r'10\.42\.0\.1', r'01a187bf-1478-', r'(?<![A-Za-z])agent1?-key(?![A-Za-z])']
    for file in ROOT.rglob('*'):
        if not file.is_file() or file.relative_to(ROOT).parts[0] in ['build', 'dist', '.git'] or file.suffix in ['.png', '.jpg', '.pf2', '.sp8bg', '.sp8rle', '.xz'] or file == Path(__file__):
            continue
        text = file.read_text(errors='replace')
        if any(re.search(p, text) for p in patterns):
            raise ValueError('Private material detected in ' + str(file.relative_to(ROOT)))
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Checks)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)


if __name__ == '__main__':
    main()
