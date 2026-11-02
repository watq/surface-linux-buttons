#!/usr/bin/env python3
"""Surface power key: lock and blank using persistent D-Bus connections."""
import argparse
import fcntl
import os
from pathlib import Path
import pwd
import signal
import struct
import time

EVENT = struct.Struct('@llHHi')
BACKLIGHT = Path('/sys/class/backlight/intel_backlight')
RESTORE = Path('/run/surface-screen-key/brightness')


def restore_backlight():
    """异常退出后也恢复原亮度；仅向固定背光节点写入校验过的整数。"""
    if RESTORE.exists():
        level = int(RESTORE.read_text())
        maximum = int((BACKLIGHT / 'max_brightness').read_text())
        if 0 <= level <= maximum:
            (BACKLIGHT / 'brightness').write_text(str(level))
            RESTORE.unlink()


def device_path():
    for node in Path('/sys/class/input').glob('event*'):
        if 'MSHW0040' not in str(node.resolve()):
            continue
        try:
            words = (node / 'device/capabilities/key').read_text().split()
            mask = int(''.join(f'{int(w, 16):016x}' for w in words), 16)
            if mask == 1 << 116:  # Dedicated physical power key only.
                return Path('/dev/input') / node.name
        except (OSError, ValueError):
            continue
    raise RuntimeError('Dedicated Surface power-key device unavailable')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--user', help='GNOME desktop account (required except for recovery)')
    parser.add_argument('--restore-backlight', action='store_true')
    args = parser.parse_args()
    if args.restore_backlight:
        restore_backlight()
        return
    if not args.user:
        parser.error('--user is required')
    restore_backlight()
    brightness_fd = os.open(BACKLIGHT / 'brightness', os.O_WRONLY)
    fd = os.open(device_path(), os.O_RDONLY | os.O_NONBLOCK)
    # Consume only this dedicated key so GNOME cannot immediately undo blanking.
    # Volume, keyboard, touch and firmware forced power-off remain independent.
    fcntl.ioctl(fd, 0x40044590, struct.pack('i', 1))  # EVIOCGRAB
    fcntl.ioctl(fd, 0x400445a0, struct.pack('i', time.CLOCK_MONOTONIC))
    # A real input event wakes GNOME's screen shield as well as the panel.
    # Shift carries no text and never authorizes an unlock.
    wake_fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(wake_fd, 0x40045564, 1)  # UI_SET_EVBIT EV_KEY
    for key in [29, 30, 42, 44, 57]:
        fcntl.ioctl(wake_fd, 0x40045565, key)  # keyboard capabilities
    info = struct.pack('80sHHHHI', b'Surface screen wake', 3, 0x045e, 0x1000, 1, 0)
    os.write(wake_fd, info + bytes(64 * 4 * 4))
    fcntl.ioctl(wake_fd, 0x5501)  # UI_DEV_CREATE
    account = pwd.getpwnam(args.user)
    RESTORE.parent.mkdir(mode=0o700, exist_ok=True)
    os.chown(RESTORE.parent, account.pw_uid, account.pw_gid)
    os.setgroups([])
    os.setgid(account.pw_gid)
    os.setuid(account.pw_uid)
    os.environ['XDG_RUNTIME_DIR'] = f'/run/user/{account.pw_uid}'
    os.environ['DBUS_SESSION_BUS_ADDRESS'] = f'unix:path=/run/user/{account.pw_uid}/bus'
    from gi.repository import Gio, GLib
    session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    system = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    proxy_flags = Gio.DBusProxyFlags.NONE
    display = Gio.DBusProxy.new_sync(session, proxy_flags, None,
        'org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
        'org.gnome.Mutter.DisplayConfig', None)
    sessions = system.call_sync('org.freedesktop.login1', '/org/freedesktop/login1',
        'org.freedesktop.login1.Manager', 'ListSessions', None, None,
        Gio.DBusCallFlags.NONE, 3000, None).unpack()[0]
    candidates = [x for x in sessions if x[1] == account.pw_uid and x[3] == 'seat0']
    if not candidates:
        raise RuntimeError('Active graphical login session unavailable')
    login = Gio.DBusProxy.new_sync(system, proxy_flags, None,
        'org.freedesktop.login1', candidates[0][4], 'org.freedesktop.login1.Session', None)
    state = {'pending': False, 'busy': False, 'queued': False,
             'requested_at': 0.0, 'last_press': -10.0, 'held': False}

    def value(proxy, name, default):
        v = proxy.get_cached_property(name)
        return v.unpack() if v is not None else default

    def brightness(level):
        """使用降权前打开的单个描述符，不授予桌面进程额外设备权限。"""
        os.lseek(brightness_fd, 0, os.SEEK_SET)
        os.write(brightness_fd, str(level).encode())

    def restore():
        if RESTORE.exists():
            brightness(int(RESTORE.read_text()))
            RESTORE.unlink()

    def finished():
        state['busy'] = False
        if state['queued']:
            state['queued'] = False
            GLib.idle_add(toggle, 'queued')

    def power(mode):
        state['pending'] = False
        action_started = state['requested_at']
        if mode == 3:
            # 锁定已确认后才灭背光。随后关闭显示管线，保留完整省电效果。
            level = int((BACKLIGHT / 'brightness').read_text())
            RESTORE.write_text(str(level))
            brightness(0)
            print(f'Backlight dark; since_press_ms={round((time.monotonic()-action_started)*1000)}; '
                  f'actual={int((BACKLIGHT / "actual_brightness").read_text())}; locked=True', flush=True)
        power_started = time.monotonic()
        print(f'Power request mode={mode}; since_press_ms={round((power_started-state["requested_at"])*1000)}', flush=True)
        def completed(bus, result):
            try:
                bus.call_finish(result)
                # DPMS 关闭后恢复设定亮度，保证键盘/触摸/电源唤醒亮度正常。
                restore()
                if mode == 0:
                    for kind, code, val in [(1, 42, 1), (0, 0, 0), (1, 42, 0), (0, 0, 0)]:
                        os.write(wake_fd, EVENT.pack(0, 0, kind, code, val))
                elapsed = round((time.monotonic() - action_started) * 1000)
                print(f'Display {"off" if mode else "on"}; action_ms={elapsed}; '
                      f'locked={value(login, "LockedHint", False)}; display_ms={round((time.monotonic()-power_started)*1000)}', flush=True)
            except GLib.Error as error:
                restore()
                print(f'Display failed: {error.message}', flush=True)
            finally:
                finished()
        session.call('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
            'org.freedesktop.DBus.Properties', 'Set',
            GLib.Variant('(ssv)', ('org.gnome.Mutter.DisplayConfig',
                                 'PowerSaveMode', GLib.Variant('i', mode))),
            None, Gio.DBusCallFlags.NONE, 3000, None, completed)

    def lock_changed(*_):
        if state['pending'] and value(login, 'LockedHint', False):
            power(3)
    login.connect('g-properties-changed', lock_changed)

    def toggle(source='physical'):
        if state['busy']:
            state['queued'] = not state['queued']
            return False
        state['busy'] = True
        state['requested_at'] = time.monotonic()
        print(f'Power action source={source}; mode={value(display, "PowerSaveMode", 0)}; '
              f'locked={value(login, "LockedHint", False)}', flush=True)
        if value(display, 'PowerSaveMode', 0) > 0:
            power(0)
        elif value(login, 'LockedHint', False):
            power(3)
        else:
            state['pending'] = True
            request_at = state['requested_at']
            def lock_result(bus, result):
                try:
                    bus.call_finish(result)
                    lock_changed()
                except GLib.Error as error:
                    if state['pending']:
                        state['pending'] = False
                        print(f'Lock failed; display left on: {error.message}', flush=True)
                        finished()
            # logind's Lock signal uses GNOME lock(false), avoiding animation.
            system.call('org.freedesktop.login1', candidates[0][4],
                'org.freedesktop.login1.Session', 'Lock', None, None,
                Gio.DBusCallFlags.NONE, 4000, None, lock_result)
            def expired():
                if state['pending'] and state['requested_at'] == request_at:
                    state['pending'] = False
                    print('Lock not confirmed; display left on', flush=True)
                    finished()
                return False
            GLib.timeout_add(4500, expired)
        return False

    def input_event(source, condition):
        if condition & (GLib.IO_HUP | GLib.IO_ERR):
            raise SystemExit('Input device disconnected; service will reconnect')
        while True:
            try:
                data = os.read(fd, EVENT.size)
            except BlockingIOError:
                break
            if len(data) != EVENT.size:
                break
            seconds, microseconds, kind, code, val = EVENT.unpack(data)
            when = seconds + microseconds / 1_000_000
            if kind == 0 and code == 3:
                state['held'] = False
            if kind != 1 or code != 116:
                continue
            if val == 1:
                if not state['held'] and when - state['last_press'] > 0.25:
                    state['held'] = True
                    state['last_press'] = when
                    # 按下即处理，不再额外等待用户松开；自动重复事件被忽略。
                    print(f'Input dispatch_ms={round((time.monotonic()-when)*1000)}', flush=True)
                    toggle()
            elif val == 0:
                state['held'] = False
        return True

    GLib.io_add_watch(fd, GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, input_event)
    # 仅同 UID/root 可触发的维护入口，复用生产动作并在日志注明测试来源。
    def exercise():
        toggle('maintenance-signal')
        return True
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, exercise)
    print(f'Ready: persistent D-Bus, uid={os.getuid()}, locked={value(login, "LockedHint", False)}', flush=True)
    GLib.MainLoop().run()


if __name__ == '__main__':
    main()
