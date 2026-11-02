#!/usr/bin/env python3
"""Unpack pinned GRUB 2.12, apply the core patch and overlay Surface sources."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent


def prepare(output):
    archive = HERE.parent / 'vendor/grub-2.12.tar.xz'
    for line in (HERE.parent / 'vendor/SHA256SUMS').read_text().splitlines():
        expected, name = line.split(maxsplit=1)
        file = HERE.parent / 'vendor' / name.strip()
        if hashlib.sha256(file.read_bytes()).hexdigest() != expected:
            raise ValueError('Source checksum mismatch: ' + name)
    output.mkdir(parents=True, exist_ok=False)
    with tarfile.open(archive) as t:
        t.extractall(output, filter='data')
    source = output / 'grub-2.12'
    patches = HERE.parent / 'vendor/ubuntu-patches'
    for line in (patches / 'series').read_text().splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patches / line.split()[0])], cwd=source, check=True, capture_output=True)
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(HERE / 'grub-core-daily.patch')], cwd=source, check=True)
    for p in (HERE / 'src').rglob('*'):
        if p.is_file():
            target = source / p.relative_to(HERE / 'src')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, target)
    print(source)
    return source


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    prepare(p.parse_args().output.resolve())
