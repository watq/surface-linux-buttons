#!/usr/bin/env python3
"""Build an independent EFI image using local UUID and optional local signing key."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
MODULES = ('normal configfile echo sleep minicmd terminal sp8_gpio sp8_system_menu font gfxterm gfxmenu '
           'all_video png jpeg serial videoinfo part_gpt part_msdos ext2 fat search search_fs_uuid '
           'search_fs_file search_label linux peimage chain boot loadenv test true gettext gzio keylayouts efifwsetup')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--grub-build', type=Path, required=True)
    p.add_argument('--root-uuid', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--key', type=Path)
    p.add_argument('--cert', type=Path)
    args = p.parse_args()
    if not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', args.root_uuid):
        p.error('Use the UUID of the filesystem containing /boot/grub/grub.cfg')
    if bool(args.key) != bool(args.cert):
        p.error('Pass both --key and --cert, or neither')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    config = (HERE / 'embedded-daily.cfg.in').read_text().replace('@ROOT_UUID@', args.root_uuid)
    (out / 'embedded-daily.cfg').write_text(config)
    subprocess.run(['grub-script-check', str(out / 'embedded-daily.cfg')], check=True)
    shutil.copytree(HERE / 'theme/Vimix', out / 'theme/Vimix')
    grafts = ['boot/grub/grub.cfg=' + str(out / 'embedded-daily.cfg'),
              'boot/grub/fonts/unicode.pf2=' + str(out / 'theme/Vimix/surface-readable-24.pf2')]
    for file in sorted((out / 'theme/Vimix').rglob('*')):
        if file.is_file() and file.name not in ['background.jpg', 'background-fast.png']:
            grafts.append('boot/grub/themes/Vimix/' + str(file.relative_to(out / 'theme/Vimix')) + '=' + str(file))
    build = args.grub_build.resolve()
    image = out / 'grub-daily.efi'
    unsigned = out / 'grub-daily-unsigned.efi'
    subprocess.run([str(build / 'grub-mkstandalone'), '-O', 'x86_64-efi', '-d', str(build / 'grub-core'),
        '--fonts=', '--locales=', '--themes=', '--modules=' + MODULES, '--install-modules=' + MODULES,
        '--sbat=' + str(HERE / 'sbat.csv'), '-o', str(unsigned), *grafts], check=True)
    if args.key:
        subprocess.run(['sbsign', '--key', str(args.key.resolve()), '--cert', str(args.cert.resolve()),
                        '--output', str(image), str(unsigned)], check=True)
        subprocess.run(['sbverify', '--cert', str(args.cert.resolve()), str(image)], check=True)
        shutil.copy2(args.cert, out / 'owner-test.pem')
    else:
        shutil.copy2(unsigned, image)
    shutil.copy2(HERE / 'README.md', out / 'steps.md')
    files = {str(f.relative_to(out)): {'bytes': f.stat().st_size,
             'sha256': hashlib.sha256(f.read_bytes()).hexdigest()} for f in out.rglob('*') if f.is_file()}
    manifest = {'release': 'surface-d1-' + hashlib.sha256(image.read_bytes()).hexdigest()[:12],
        'kind': 'daily-D1', 'image': image.name, 'steps': 'steps.md', 'files': files, 'filesystem_uuid': args.root_uuid,
        'signed_image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(), 'signed': bool(args.key),
        'theme': 'Vimix / left-aligned centered block / 48-36-24',
        'menu_physical_acceptance': 'New locally built image; hardware acceptance pending',
        'current_system_config_loaded_each_boot': True, 'kernel_versions_frozen': False}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'release': manifest['release'], 'image': str(image), 'signed': bool(args.key)}))


if __name__ == '__main__':
    main()

