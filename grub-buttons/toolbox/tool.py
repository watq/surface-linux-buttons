#!/usr/bin/env python3
"""User-owned Surface GRUB versions, reversible daily entry and log history."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import importlib.util
import fcntl
import platform

LABEL = re.compile(r'[a-zA-Z0-9][a-zA-Z0-9_.-]{0,100}')


def now():
    return datetime.datetime.now().astimezone().isoformat()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def append_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with path.open('a', encoding='utf-8') as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        f.write(json.dumps(value, ensure_ascii=False) + '\n')
        f.flush(); os.fsync(f.fileno())


def acceptance(base, label, manifest):
    path = base / 'acceptance' / (label + '.json')
    if path.is_file() and not path.is_symlink():
        receipt = read(path)
        if receipt.get('accepted') and receipt.get('image_sha256') == manifest.get('signed_image_sha256'):
            return '用户实机验收通过（' + receipt.get('scope', '该版本') + '）'
    return manifest['menu_physical_acceptance']


def atomic(path, data):
    temporary = path.with_name(path.name + '.new-' + str(os.getpid()))
    with temporary.open('x', encoding='utf-8') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(temporary, path)


def atomic_json(path, value):
    atomic(path, json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def release_path(base, label):
    if not isinstance(label, str) or not LABEL.fullmatch(label):
        raise ValueError('Invalid release label')
    path = base / 'releases' / label
    if path.is_symlink() or not path.is_dir():
        raise ValueError('Release directory missing or symbolic')
    return path


def validate_release(base, label):
    index = read(base / 'releases.json')
    expected = index['releases'].get(label)
    if not expected:
        raise ValueError('Release is not in the local preserved index')
    release = release_path(base, label)
    manifest_file = release / 'manifest.json'
    if manifest_file.is_symlink() or digest(manifest_file) != expected['manifest_sha256']:
        raise ValueError('Release manifest changed')
    manifest = read(manifest_file)
    if len(manifest['files']) > 500:
        raise ValueError('Unexpected manifest size')
    for name, metadata in manifest['files'].items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or not relative.parts:
            raise ValueError('Manifest path is outside the release')
        path = release / relative
        if path.is_symlink() or any((release / Path(*relative.parts[:i])).is_symlink()
                                    for i in range(1, len(relative.parts))):
            raise ValueError('Symbolic artifact path')
        if not path.is_file() or path.stat().st_size != metadata['bytes'] or digest(path) != metadata['sha256']:
            raise ValueError('Artifact changed: ' + name)
    return release, manifest


def launch_text(base, label, enabled):
    install = read(base / 'installation.json')
    uuid = install['filesystem_uuid']
    if not re.fullmatch(r'[0-9a-fA-F-]{36}', uuid):
        raise ValueError('Invalid root filesystem UUID')
    header = '# SP8-GRUB-KEYS-MANUAL-LAUNCH-V1\nset timeout=-1\nset default=0\n'
    header += 'search --no-floppy --fs-uuid --set=sp8_m1_root ' + uuid + '\n'
    header += 'export sp8_m1_root\nterminal_input console\n'
    if enabled:
        release, manifest = validate_release(base, label)
        if manifest.get('kind') == 'daily-D1':
            raise ValueError('Use daily-enable for a daily release; the M1 manual entry stays separate')
        image = str(release / manifest['image'])
        # GRUB config paths here are generated from fixed user-owned ASCII
        # directories, never shell commands or caller-provided expressions.
        if not re.fullmatch(r'/[A-Za-z0-9_./-]+', image):
            raise ValueError('Unsupported GRUB release path')
        header += "menuentry '1. M1 three-key menu - existing Vimix theme' {\n"
        header += '  chainloader ($sp8_m1_root)' + image + '\n}\n'
    header += "menuentry '2. Return to original Ubuntu menu' {\n"
    header += '  configfile ($sp8_m1_root)/boot/grub/grub.cfg\n}\n'
    return header


def set_active(base, label, enabled, reason):
    active_file = base / 'active.json'
    old = read(active_file) if active_file.exists() else None
    text = launch_text(base, label, enabled)
    backup = base / 'backups' / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '-' + str(os.getpid()))
    backup.mkdir(parents=True, mode=0o700)
    fingerprints = []
    for name in ['active.json', 'active-launch.cfg']:
        p = base / name
        if p.exists():
            data = p.read_bytes()
            with (backup / name).open('xb') as f:
                f.write(data)
            fingerprints.append({'source': str(p), 'backup': str(backup / name), 'sha256': hashlib.sha256(data).hexdigest()})
    event = {'at': now(), 'reason': reason, 'old': old, 'new_release': label, 'enabled': enabled,
             'backup': str(backup), 'files': fingerprints, 'system_boot_changed': False}
    atomic(base / 'active-launch.cfg', text)
    previous = old.get('release') if old and old.get('release') != label else (old or {}).get('previous_release')
    atomic_json(active_file, {'release': label, 'enabled': enabled, 'previous_release': previous,
                             'updated_at': now(), 'reason': reason})
    atomic_json(backup / 'operation.json', event)
    append_json(base / 'operations.jsonl', event)
    return event


def daily_hook_text(base):
    uuid = read(base / 'installation.json')['filesystem_uuid']
    if not re.fullmatch(r'[0-9a-fA-F-]{36}', uuid) or not re.fullmatch(r'/[A-Za-z0-9_./-]+', str(base)):
        raise ValueError('Unsupported daily hook path or UUID')
    return ('''# SP8-GRUB-KEYS-DAILY-HOOK-V1
# User data is separate from package EFI files. Missing/failed entry returns here.
if [ -z "$sp8_daily_active" -a -z "$sp8_daily_skip" ]; then
  set sp8_daily_skip=1
  export sp8_daily_skip
  if search --no-floppy --fs-uuid --set=sp8_daily_hook_root ''' + uuid + '''; then
    if [ -f ($sp8_daily_hook_root)''' + str(base) + '''/daily-active.cfg ]; then
      source ($sp8_daily_hook_root)''' + str(base) + '''/daily-active.cfg
    fi
  fi
fi
''')


def daily_launch_text(base, label, enabled):
    text = '# SP8-GRUB-KEYS-DAILY-POINTER-V1\n'
    if not enabled:
        return text + '# Disabled: continue the original system menu.\n'
    release, manifest = validate_release(base, label)
    if manifest.get('kind') != 'daily-D1':
        raise ValueError('Daily entry requires a verified daily release')
    image = str(release / manifest['image'])
    if not re.fullmatch(r'/[A-Za-z0-9_./-]+', image):
        raise ValueError('Unsupported daily image path')
    return (text + 'if [ -f ($sp8_daily_hook_root)' + image + ' ]; then\n'
            '  if chainloader ($sp8_daily_hook_root)' + image + '; then\n'
            '    boot\n'
            '  fi\n'
            'fi\n')


def set_daily(base, label, enabled, reason):
    old_path = base / 'daily-state.json'
    old = read(old_path) if old_path.exists() else None
    versions = read(base / 'releases.json')['releases']
    if enabled and versions.get(label, {}).get('daily_activation_blocked_reason'):
        raise ValueError('该历史版已保留但禁止日常启用：' + versions[label]['daily_activation_blocked_reason'])
    text = daily_launch_text(base, label, enabled)
    directory = base / 'backups' / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '-daily-' + str(os.getpid()))
    directory.mkdir(parents=True, mode=0o700)
    files = []
    for name in ['daily-state.json', 'daily-active.cfg']:
        p = base / name
        if p.exists():
            data = p.read_bytes()
            with (directory / name).open('xb') as f:
                f.write(data)
            files.append({'source': str(p), 'backup': str(directory / name), 'sha256': hashlib.sha256(data).hexdigest()})
    previous = old.get('release') if old and old.get('release') != label else (old or {}).get('previous_release')
    if previous and versions.get(previous, {}).get('daily_activation_blocked_reason'):
        previous = None
    state = {'release': label, 'enabled': enabled, 'previous_release': previous,
             'updated_at': now(), 'reason': reason, 'custom_hook': '/boot/grub/custom.cfg'}
    atomic(base / 'daily-active.cfg', text)
    atomic_json(old_path, state)
    event = {'at': now(), 'mode': 'daily', 'reason': reason, 'old': old, 'new': state,
             'backup': str(directory), 'files': files, 'system_EFI_or_firmware_changed': False}
    atomic_json(directory / 'operation.json', event)
    append_json(base / 'operations.jsonl', event)
    return event


def collect_logs(base):
    spec = importlib.util.spec_from_file_location('surface_grub_log_store', Path(__file__).with_name('log-store.py'))
    if not spec or not spec.loader:
        raise ValueError('Log collector is not installed')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    result = module.collect(base)
    append_json(base / 'logs/collections.jsonl', result)
    atomic_json(base / 'logs/latest-collection.json', result)
    return result


def package_versions():
    p = subprocess.run(['dpkg-query', '-W', '-f=${Package}\t${Version}\n',
                        'grub-common', 'grub2-common', 'grub-efi-amd64-bin',
                        'grub-efi-amd64-signed', 'shim-signed'], capture_output=True, text=True, timeout=10)
    if p.returncode:
        return {'error': p.stderr.strip()}
    return dict(line.split('\t', 1) for line in p.stdout.splitlines() if '\t' in line)


def check(base, system=True):
    active = read(base / 'active.json')
    release, manifest = validate_release(base, active['release'])
    issues, notes = [], []
    expected = launch_text(base, active['release'], active['enabled'])
    if (base / 'active-launch.cfg').read_text() != expected:
        issues.append('用户级手动入口与所选版本不一致；可运行 repair 重建用户级入口。')
    installation = read(base / 'installation.json')
    if system:
        shortcut = Path(installation['shortcut'])
        if not shortcut.is_file() or shortcut.read_text() != installation['shortcut_text']:
            issues.append('根目录手动快捷入口缺失或改变；工具不会自动覆盖，请按说明核验。')
        current = package_versions()
        if current != installation['package_versions_at_install']:
            notes.append('系统 GRUB/shim 软件包版本变化；独立版本未被替换，启动前需重新核验签名兼容。')
        verifier = subprocess.run(['sbverify', '--cert', str(release / 'owner-test.pem'), str(release / manifest['image'])],
                                  capture_output=True, text=True, timeout=15)
        if verifier.returncode:
            issues.append('现有用户证书不能验证该EFI签名。')
        original = Path(manifest['theme_source'])
        changed = []
        for relative, expected_theme in manifest['original_theme_hashes'].items():
            p = original / relative
            if not p.is_file() or digest(p) != expected_theme:
                changed.append(relative)
        if changed:
            delivery = read(base / 'daily-installation.json') if (base / 'daily-installation.json').exists() else {}
            if changed == ['theme.txt'] and digest(original / 'theme.txt') == delivery.get('theme_delivery_hashes', {}).get(str(original / 'theme.txt')):
                notes.append('原GRUB字体已按用户要求放大；已验收M1保留当时的Vimix副本。')
            else:
                notes.append('系统原主题已有变化；本版本保全的Vimix副本保留，可在下一个版本中更新。')
    return {'author': 'surface-grub-keys user tool', 'checked_at': now(), 'integrity_passed': not issues,
            'release': active['release'], 'enabled': active['enabled'], 'theme': manifest['theme'],
            'image_sha256': digest(release / manifest['image']), 'issues': issues, 'notes': notes,
            'G1_physical_capture_verified': manifest['G1_capture_verified'],
            'this_menu_physical_acceptance': acceptance(base, active['release'], manifest),
            'system_boot_or_firmware_changed': False, 'updates_blocked': False,
            'mode': 'manual independent image only'}


def check_daily(base, system=True):
    path = base / 'daily-state.json'
    if not path.exists():
        return {'installed': False, 'enabled': False, 'issues': [], 'notes': [], 'integrity_passed': True}
    state = read(path)
    release, manifest = validate_release(base, state['release'])
    issues, notes = [], []
    if (base / 'daily-active.cfg').read_text() != daily_launch_text(base, state['release'], state['enabled']):
        issues.append('日常用户入口与版本不一致；daily-repair 可保留旧指针后修复。')
    if system:
        hook = Path(state['custom_hook'])
        if not hook.is_file() or hook.is_symlink() or hook.read_text() != daily_hook_text(base):
            issues.append('日常 GRUB 扩展入口缺失或改变；检查只报告，保留现状。')
        cfg = Path('/boot/grub/grub.cfg')
        try:
            has_hook = cfg.is_file() and 'source ${config_directory}/custom.cfg' in cfg.read_text()
        except PermissionError:
            has_hook = 'custom.cfg' in Path('/etc/grub.d/41_custom').read_text()
            notes.append('系统生成菜单只允许管理员读取；用户版本/入口已检查，管理员可另核菜单内容。')
        if not has_hook:
            issues.append('当前系统配置没有 custom.cfg 扩展点。')
        if manifest.get('signed'):
            verification = subprocess.run(['sbverify', '--cert', str(release / 'owner-test.pem'),
                                           str(release / manifest['image'])], capture_output=True, text=True, timeout=15)
            if verification.returncode:
                issues.append('日常EFI的本地证书签名核验失败。')
        else:
            notes.append('未签名的本地构建；Secure Boot 下必须先完成签名与信任登记。')
        for name, expected in read(base / 'daily-installation.json').get('theme_delivery_hashes', {}).items():
            path = Path(name)
            if not path.is_file() or digest(path) != expected:
                notes.append('系统主题/放大字体已变化：' + name + '；签名日常版保全的字体不受该覆盖影响。')
    return {'installed': True, 'enabled': state['enabled'], 'release': state['release'],
            'theme': manifest['theme'], 'image_sha256': digest(release / manifest['image']),
            'integrity_passed': not issues, 'issues': issues, 'notes': notes,
            'menu_physical_acceptance': acceptance(base, state['release'], manifest),
            'current_system_config_loaded_each_boot': True, 'kernel_versions_frozen': False}


def print_check(report):
    print('版本：' + report['release'] + '；主题：' + report['theme'])
    print('完整性检查：' + ('通过' if report['integrity_passed'] else '需要处理'))
    print('手动入口：' + ('已启用' if report['enabled'] else '已停用，仅返回原菜单'))
    print('GPIO取键已实机确认；本菜单实机验收：' + report['this_menu_physical_acceptance'])
    for line in report['issues'] + report['notes']:
        print(line)
    print('原GRUB按c，然后输入：configfile /gm.cfg')
    daily = report.get('daily', {})
    if daily.get('installed'):
        print('日常三键入口：' + ('已启用' if daily['enabled'] else '已停用') + '；版本：' + daily['release'])
        print('日常版实机状态：' + daily['menu_physical_acceptance'])
        for line in daily['issues'] + daily['notes']:
            print(line)
    print('启动日志：' + report.get('boot_log_directory', '尚未采集'))


def cli():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, default=Path.home() / '.local/share/surface-grub-keys')
    parser.add_argument('action', nargs='?', default='menu', choices=['menu', 'check', 'steps', 'activate', 'rollback', 'disable', 'repair',
                        'logs', 'daily-enable', 'daily-disable', 'daily-rollback', 'daily-repair', 'daily-steps'])
    parser.add_argument('version', nargs='?')
    parser.add_argument('--record', action='store_true')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    base = args.base.resolve()
    action = args.action
    if action == 'menu':
        print('Surface GRUB三键工具箱\n1 检查版本并保存日志\n2 日常三键操作说明\n3 停用日常三键菜单\n4 回退日常版本（无上一版时停用）\n5 启用日常三键菜单\n6 收集/查看启动日志\n7 日常三键操作说明\n0 退出')
        selected = input('选择：').strip()
        action = {'1': 'check', '2': 'daily-steps', '3': 'daily-disable', '4': 'daily-rollback',
                  '5': 'daily-enable', '6': 'logs', '7': 'daily-steps'}.get(selected)
        if not action:
            return 0
    if action == 'check':
        try:
            report = check_daily(base)
        except (ValueError, OSError, KeyError) as error:
            report = {'integrity_passed': False, 'issues': [str(error)]}
        if args.record:
            atomic_json(base / 'last-check.json', report)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0 if report['integrity_passed'] else 1
    if action == 'logs':
        result = collect_logs(base)
        if args.json:
            print(json.dumps(result, ensure_ascii=False, indent=2))
        else:
            print('启动日志原件保留；副本目录：' + str(base / 'logs/boot'))
            print('新增副本：' + str(len(result['new_copies'])))
            for row in result['all_logs']:
                print(Path(row['source']).name + '：' + row['version'] + '；END=' + str(row['complete_end_logged'])
                      + '；原因=' + str(row['end_reason']) + '；错误=' + str(len(row['errors'])))
        return 0
    if action.startswith('daily-'):
        state = read(base / 'daily-state.json')
        if action == 'daily-steps':
            release, manifest = validate_release(base, state['release'])
            print((release / manifest['steps']).read_text())
            return 0
        label = args.version or state['release']
        enabled = action != 'daily-disable'
        if action == 'daily-rollback':
            label = state.get('previous_release') or state['release']
            enabled = bool(state.get('previous_release'))
        if action == 'daily-repair':
            enabled = state['enabled']
        event = set_daily(base, label, enabled, action)
        print('日常三键菜单：' + ('已启用' if enabled else '已停用，继续原系统菜单'))
        print('改前指针保留：' + event['backup'])
        return 0
    active = read(base / 'active.json')
    if action == 'steps':
        release, manifest = validate_release(base, active['release'])
        print((release / manifest['steps']).read_text())
        return 0
    if action in ['disable', 'rollback']:
        label = active.get('previous_release') if action == 'rollback' else None
        event = set_active(base, label or active['release'], bool(label), action)
        print('已回退/停用用户级手动入口；原GRUB正常启动配置保持不变。')
        print('保留版本与备份：' + event['backup'])
        return 0
    if action == 'activate':
        label = args.version or active['release']
        event = set_active(base, label, True, 'manual_activate')
        print('已选版本：' + event['new_release'] + '；进入命令：configfile /gm.cfg')
        return 0
    if action == 'repair':
        event = set_active(base, active['release'], active['enabled'], 'repair_user_pointer_only')
        print('用户级手动入口已重建，改前副本：' + event['backup'])
        return 0
    return 0


if __name__ == '__main__':
    try:
        sys.exit(cli())
    except (ValueError, OSError, KeyError) as error:
        print('工具操作未完成：' + str(error), file=sys.stderr)
        sys.exit(1)
