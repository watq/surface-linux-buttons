#!/usr/bin/env python3
"""Install a new user toolbox, or explicitly add a nonoverwriting GRUB hook."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent


def load_tool():
    spec = importlib.util.spec_from_file_location('surface_tool', HERE / 'toolbox/tool.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--release', type=Path)
    p.add_argument('--base', type=Path, default=Path.home() / '.local/share/surface-grub-keys')
    p.add_argument('--install-hook', action='store_true')
    args = p.parse_args()
    base = args.base.absolute()
    if not re.fullmatch(r'/[A-Za-z0-9_./-]+', str(base)) or '..' in base.parts or any(x.is_symlink() for x in [base, *base.parents]):
        p.error('Use an absolute ASCII path without symbolic ancestors')
    tool = load_tool()
    if args.install_hook:
        if os.geteuid() != 0:
            p.error('--install-hook requires sudo')
        hook = Path('/boot/grub/custom.cfg')
        if hook.exists() or hook.is_symlink():
            p.error('custom.cfg already exists; preserve it and integrate the documented hook manually')
        cfg = Path('/boot/grub/grub.cfg')
        if 'source ${config_directory}/custom.cfg' not in cfg.read_text():
            p.error('Current generated GRUB config does not source custom.cfg')
        tool.check_daily(base, system=False)
        text = tool.daily_hook_text(base)
        # Validate before installing the extension point.
        subprocess.run(['grub-script-check', '/dev/stdin'], input=text, text=True, check=True)
        with hook.open('x') as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        hook.chmod(0o644)
        print('Added disabled hook:', hook)
        return
    if os.geteuid() == 0:
        p.error('Install the toolbox from the GNOME desktop user, without sudo')
    if not args.release:
        p.error('--release is required')
    release = args.release.resolve()
    manifest = tool.read(release / 'manifest.json')
    uuid = manifest['filesystem_uuid']
    if not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', uuid):
        p.error('Invalid filesystem UUID')
    for name, row in manifest['files'].items():
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            p.error('Invalid manifest path')
        file = release / name
        if file.is_symlink() or not file.is_file() or any(x.is_symlink() for x in file.parents) or tool.digest(file) != row['sha256'] or file.stat().st_size != row['bytes']:
            p.error('Release checksum mismatch: ' + name)
    if tool.digest(release / manifest['image']) != manifest['signed_image_sha256']:
        p.error('Image checksum mismatch')
    label = manifest['release']
    if not tool.LABEL.fullmatch(label):
        p.error('Invalid release label')
    # A current Surface installation must never be replaced by a public installer.
    base.mkdir(parents=True, mode=0o755, exist_ok=False)
    (base / 'releases').mkdir(mode=0o755)
    target = base / 'releases' / label
    shutil.copytree(release, target)
    for name in ['tool.py', 'gui.py', 'log-store.py']:
        shutil.copy2(HERE / 'toolbox' / name, base / name)
    tool.atomic_json(base / 'installation.json', {'filesystem_uuid': uuid})
    tool.atomic_json(base / 'daily-installation.json', {'theme_delivery_hashes': {}})
    tool.atomic_json(base / 'releases.json', {'releases': {label: {
        'kind': 'daily-D1', 'manifest_sha256': tool.digest(target / 'manifest.json'),
        'signed_image_sha256': manifest['signed_image_sha256']}}})
    tool.set_daily(base, label, False, 'public_install_disabled')
    desktop = Path.home() / '.local/share/applications/surface-grub-keys.desktop'
    if desktop.exists() or desktop.is_symlink():
        print('Existing desktop launcher preserved; launch the GUI directly:', base / 'gui.py')
    else:
        desktop.parent.mkdir(parents=True, exist_ok=True)
        with desktop.open('x') as f:
            f.write('[Desktop Entry]\nType=Application\nName=Surface GRUB三键工具箱\n'
                    f'Exec=/usr/bin/python3 {base}/gui.py --base {base}\n'
                    'Terminal=false\nCategories=System;\nStartupWMClass=surface-grub-keys\n')
    print('Installed, daily entry disabled:', base)


if __name__ == '__main__':
    main()

