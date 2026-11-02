#!/usr/bin/env python3
"""Install or restore the Surface power-key service, preserving previous files."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
SERVICE = 'surface-screen-key.service'
SCHEMA = 'org.gnome.settings-daemon.plugins.power'
SETTINGS = {'power-button-action': "'nothing'",
            'sleep-inactive-ac-type': "'nothing'",
            'sleep-inactive-battery-type': "'nothing'"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def desktop(user, *args):
    account = pwd.getpwnam(user)
    return subprocess.run(['runuser', '-u', user, '--', 'env',
        f'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{account.pw_uid}/bus',
        'gsettings', *args], check=True, text=True, capture_output=True).stdout.strip()


def targets(user):
    unit = (HERE / 'systemd/surface-screen-key.service.in').read_text().replace('@USER@', user)
    policy = '[Login]\nHandlePowerKey=ignore\nIdleAction=ignore\n'
    return {'usr/local/libexec/surface-screen-key.py': (HERE / 'surface-screen-key.py').read_bytes(),
            'etc/systemd/system/' + SERVICE: unit.encode(),
            'etc/systemd/logind.conf.d/60-surface-screen-key.conf': policy.encode()}


def run_system(*args):
    return subprocess.run(['systemctl', *args], check=True, capture_output=True, text=True)


def apply(root, user):
    live = root == Path('/')
    if live:
        if os.geteuid() != 0 or pwd.getpwnam(user).pw_uid == 0:
            raise ValueError('Run as root and select a non-root GNOME account')
        # Verify all desktop settings before touching a running deployment.
        old_settings = {key: desktop(user, 'get', SCHEMA, key) for key in SETTINGS}
        enabled = subprocess.run(['systemctl', 'is-enabled', SERVICE], capture_output=True, text=True).stdout.strip()
        active = subprocess.run(['systemctl', 'is-active', SERVICE], capture_output=True, text=True).stdout.strip()
    else:
        old_settings, enabled, active = {}, '', ''
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    backup = root / 'var/lib/surface-screen-key/backups' / stamp
    backup.mkdir(parents=True, mode=0o700)
    receipt = {'user': user, 'files': [], 'gsettings': old_settings,
               'previous_enabled': enabled, 'previous_active': active, 'complete': False}
    expected = targets(user)
    for name, data in expected.items():
        path = root / name
        if path.is_symlink() or any(p.is_symlink() for p in path.parents):
            raise ValueError('Refusing symbolic destination: ' + name)
        saved = backup / name
        existed = path.exists()
        if existed:
            if not path.is_file():
                raise ValueError('Destination is not a regular file: ' + name)
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, saved)
        receipt['files'].append({'path': name, 'existed': existed,
            'previous_sha256': digest(saved) if existed else None,
            'installed_sha256': hashlib.sha256(data).hexdigest()})
    receipt_path = backup / 'receipt.json'
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    if live:
        run_system('stop', SERVICE) if active == 'active' else None
    for name, data in expected.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + '.surface-new')
        with temporary.open('xb') as f:
            f.write(data)
        temporary.chmod(0o644)
        os.replace(temporary, path)
    if live:
        run_system('daemon-reload')
        for key, value in SETTINGS.items():
            desktop(user, 'set', SCHEMA, key, value)
        # Ask logind to reload configuration without restarting login sessions.
        run_system('kill', '--kill-who=main', '--signal=HUP', 'systemd-logind.service')
        run_system('enable', '--now', SERVICE)
    receipt['complete'] = True
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    print(receipt_path)


def restore(root, receipt_path):
    receipt = json.loads(receipt_path.read_text())
    backup = receipt_path.parent
    # Check all files first; later administrator edits must never be overwritten.
    for row in receipt['files']:
        name = row['path']
        if name not in targets(receipt['user']):
            raise ValueError('Unknown backup target')
        path = root / name
        if path.is_symlink() or any(p.is_symlink() for p in path.parents) or not path.is_file() or digest(path) != row['installed_sha256']:
            raise ValueError('File changed after installation: ' + name)
        if row['existed'] and digest(backup / name) != row['previous_sha256']:
            raise ValueError('Backup changed: ' + name)
    live = root == Path('/')
    if live:
        run_system('disable', '--now', SERVICE)
    for row in receipt['files']:
        path = root / row['path']
        if row['existed']:
            shutil.copy2(backup / row['path'], path)
        else:
            path.unlink()
    if live:
        run_system('daemon-reload')
        for key, value in receipt['gsettings'].items():
            desktop(receipt['user'], 'set', SCHEMA, key, value)
        run_system('kill', '--kill-who=main', '--signal=HUP', 'systemd-logind.service')
        if receipt['previous_enabled'] == 'enabled':
            run_system('enable', SERVICE)
        if receipt['previous_active'] == 'active':
            run_system('start', SERVICE)
    print('Restored; original backup retained:', backup)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--user')
    p.add_argument('--root', type=Path, default=Path('/'), help='Offline staging root; no services or desktop settings are changed')
    p.add_argument('--apply', action='store_true')
    p.add_argument('--restore', type=Path)
    args = p.parse_args()
    root = args.root.resolve()
    if args.restore:
        if not args.apply:
            p.error('--restore requires --apply')
        restore(root, args.restore)
    else:
        if not args.user or not re.fullmatch(r'[a-z_][a-z0-9_-]*', args.user):
            p.error('Select the GNOME desktop account with --user')
        if args.apply:
            apply(root, args.user)
        else:
            print(json.dumps({'write_files': list(targets(args.user)), 'desktop_settings': SETTINGS,
                              'user': args.user, 'apply': False}, indent=2))


if __name__ == '__main__':
    main()

