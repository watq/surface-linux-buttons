#!/usr/bin/env python3
"""Read-only ESP log collection into a non-overwriting user history store."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import stat


def now():
    return datetime.datetime.now().astimezone().isoformat()


def fingerprint(data):
    return hashlib.sha256(data).hexdigest()


def read_regular(path, limit):
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_size > limit:
            raise ValueError('Unexpected log file type or size: ' + str(path))
        with os.fdopen(fd, 'rb', closefd=False) as f:
            data = f.read(limit + 1)
        after = os.fstat(fd)
        if len(data) != before.st_size or len(data) > limit or (
                before.st_ino, before.st_size, before.st_mtime_ns) != (
                after.st_ino, after.st_size, after.st_mtime_ns):
            raise ValueError('Source changed during collection: ' + str(path))
        return data, after
    finally:
        os.close(fd)


def summarize(data):
    text = data.decode('utf-8', errors='replace')
    # Raw bytes remain unchanged in the store. LF is the evidence line unit.
    lines = [line.rstrip('\r') for line in text.split('\n')]
    matched = lambda pattern: [{'line': i + 1, 'text': line} for i, line in enumerate(lines)
                               if re.match(pattern, line)]
    starts = matched(r'^=== SP8 GRUB GPIO (M1|D1) START ===$')
    ends = matched(r'^=== SP8 GRUB GPIO (M1|D1) END reason=')
    complete = bool(ends and matched(r'^LOG END complete=1$'))
    reason = re.search(r'reason=([^ ]+)', ends[-1]['text']).group(1) if ends else None
    errors = matched(r'^D1 ERROR ')
    classification = ('missing_END_or_interrupted' if not complete else
                      'handoff_to_loader' if reason == 'system_boot' else
                      'configuration_failed_return_original' if reason in ['config_import_failed', 'root_not_found'] else
                      'closed_after_logged_errors' if errors else 'normal_return_or_shutdown_of_module')
    return {'version': 'D1' if any(' D1 START ' in x['text'] for x in starts) else 'M1' if starts else 'unknown',
            'GPIO_events': matched(r'^GPIO EVENT '),
            'GRUB_deliveries': matched(r'^M1 DELIVER '),
            'selections': matched(r'^(?:M1 MENU SELECT|D1 SYSTEM SELECT) '),
            'phases': matched(r'^D1 PHASE '),
            'errors': errors,
            'GPIO_stops': matched(r'^M1 STOP '),
            'boot_handoffs': matched(r'^D1 BOOT HANDOFF '),
            'boot_returns': matched(r'^D1 BOOT RETURN '),
            'complete_end_logged': complete, 'end_reason': reason,
            'classification': classification,
            'successful_OS_boot_proven_by_this_log': False,
            'EFI_Close_success_proven_by_this_log': False,
            'automatic_physical_acceptance': False}


def collect(base, source=Path('/boot/efi/SP8-KEYDIAG')):
    source = Path(source)
    directory = base / 'logs/boot'
    result = {'collected_at': now(), 'source': str(source), 'source_logs_modified': False,
              'new_copies': [], 'all_logs': [], 'issues': []}
    if source.is_symlink():
        raise ValueError('Log source directory is symbolic')
    if not source.is_dir():
        result['issues'].append('启动日志目录当前不可用；保留既有副本。')
        return result
    marker, _ = read_regular(source / 'OWNER.ID', 128)
    if marker != b'SP8-KEYDIAG-LOG-V1\n':
        raise ValueError('Exact existing ESP owner marker required')
    paths = sorted(p for p in source.iterdir() if re.fullmatch(r'GM[0-9]{4}\.LOG', p.name))
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    for path in paths:
        data, info = read_regular(path, 4 * 1024 * 1024)
        sha = fingerprint(data)
        saved = directory / (path.stem + '-' + sha + '.LOG')
        if saved.exists() or saved.is_symlink():
            if saved.is_symlink() or read_regular(saved, 4 * 1024 * 1024)[0] != data:
                raise ValueError('Existing stored copy changed: ' + str(saved))
        else:
            with saved.open('xb') as f:
                f.write(data); f.flush(); os.fsync(f.fileno())
            saved.chmod(0o600)
            result['new_copies'].append(str(saved))
        check, again = read_regular(path, 4 * 1024 * 1024)
        if check != data or again.st_mtime_ns != info.st_mtime_ns:
            raise ValueError('Source changed after copy; saved bytes retained: ' + str(path))
        row = {'source': str(path), 'copy': str(saved), 'bytes': len(data), 'sha256': sha,
               'source_mtime_ns': info.st_mtime_ns, **summarize(data)}
        result['all_logs'].append(row)
    if read_regular(source / 'OWNER.ID', 128)[0] != marker:
        raise ValueError('Owner marker changed during collection')
    return result
