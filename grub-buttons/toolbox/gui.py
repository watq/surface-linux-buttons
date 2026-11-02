#!/usr/bin/python3
"""Touch-friendly GTK toolbox for preserved user-owned GRUB releases."""
import argparse
import concurrent.futures
import datetime
import importlib.util
import json
import os
from pathlib import Path
import sys
import traceback

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gio, GLib, Gdk, Pango


class Toolbox:
    def __init__(self, base, system=True):
        self.base = Path(base).resolve()
        self.system = system
        spec = importlib.util.spec_from_file_location('surface_grub_user_tool', self.base / 'tool.py')
        self.tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.tool)

    def snapshot(self):
        state = self.tool.read(self.base / 'daily-state.json')
        index = self.tool.read(self.base / 'releases.json')
        versions = []
        for label, metadata in reversed(list(index['releases'].items())):
            if metadata.get('kind') != 'daily-D1':
                continue
            try:
                release, manifest = self.tool.validate_release(self.base, label)
                versions.append({'label': label, 'theme': manifest.get('theme', 'Vimix'),
                                 'valid': True, 'active': state['release'] == label,
                                 'blocked': metadata.get('daily_activation_blocked_reason'),
                                 'acceptance': self.tool.acceptance(self.base, label, manifest),
                                 'created': metadata.get('created_at', '')})
            except (ValueError, OSError, KeyError) as error:
                versions.append({'label': label, 'valid': False, 'active': state['release'] == label,
                                 'blocked': str(error), 'theme': '文件检查未通过', 'acceptance': ''})
        report = self.tool.check_daily(self.base, system=self.system)
        logs = self.base / 'logs/boot'
        log_files = sorted((p for p in logs.glob('*.LOG') if p.is_file() and not p.is_symlink()),
                           key=lambda p: p.stat().st_mtime_ns, reverse=True) if logs.exists() else []
        return {'state': state, 'versions': versions, 'check': report,
                'logs': [{'path': str(p), 'name': p.name, 'bytes': p.stat().st_size} for p in log_files]}

    def run(self, action, label=None):
        if action == 'refresh':
            return self.snapshot(), None
        if action == 'check':
            report = self.tool.check_daily(self.base, system=self.system)
            try:
                report['log_collection'] = self.tool.collect_logs(self.base) if self.system else {'new_copies': []}
            except (ValueError, OSError, KeyError) as error:
                report['log_collection'] = {'error': str(error), 'source_logs_modified': False}
            self.tool.atomic_json(self.base / 'last-check.json', report)
            self.tool.append_json(self.base / 'logs/checks.jsonl', report)
            return self.snapshot(), report
        if action == 'logs':
            collected = self.tool.collect_logs(self.base) if self.system else {'new_copies': []}
            return self.snapshot(), collected
        if action in ['enable', 'disable', 'rollback', 'repair']:
            state = self.tool.read(self.base / 'daily-state.json')
            chosen = label or state['release']
            enabled = action != 'disable'
            if action == 'rollback':
                chosen = state.get('previous_release') or state['release']
                enabled = bool(state.get('previous_release'))
            if action == 'repair':
                chosen, enabled = state['release'], state['enabled']
            event = self.tool.set_daily(self.base, chosen, enabled, 'gui_' + action)
            return self.snapshot(), event
        if action == 'steps':
            state = self.tool.read(self.base / 'daily-state.json')
            release, manifest = self.tool.validate_release(self.base, label or state['release'])
            return self.snapshot(), (release / manifest['steps']).read_text()
        if action == 'history':
            return self.run('steps', label)
        raise ValueError('未知操作')

    def read_log(self, path):
        path = Path(path)
        if path.parent != self.base / 'logs/boot' or path.is_symlink() or not path.is_file():
            raise ValueError('只读取工具保存的日志副本')
        if path.stat().st_size > 4 * 1024 * 1024:
            raise ValueError('日志超过读取上限')
        return path.read_text(errors='replace')


CSS = b'''
window { background-color: #111820; color: #e6edf5; }
headerbar { background-image: none; background-color: #17222e; color: #e6edf5; border-color: #293849; }
.surface-status { font-size: 21px; font-weight: 700; }
.surface-hint { color: #a8b9ca; }
.surface-panel { padding: 20px; border-radius: 14px; background-color: #1b2836; }
button { background-image: none; background-color: #253649; color: #e6edf5; border: 1px solid #3b526b; border-radius: 10px; }
button:hover { background-color: #324b63; border-color: #7ab9d6; }
button:active { background-color: #143b4b; }
button:focus { border-color: #6ee7d2; }
button:disabled { background-color: #1b2633; color: #66798d; border-color: #2a3b4b; }
button.surface-action { min-height: 48px; min-width: 105px; padding: 8px 16px; font-size: 16px; }
button.suggested-action { background-color: #196f71; color: #ffffff; border-color: #3faeb0; }
button.suggested-action:hover { background-color: #248b8d; }
button.suggested-action:disabled { background-color: #28474f; color: #8ba2b2; }
notebook > header { background-color: #17222e; }
notebook > header tab { padding: 12px 18px; color: #a8b9ca; }
notebook > header tab:checked { color: #6ee7d2; background-color: #1b2836; }
list, row { background-color: #151f2b; color: #e6edf5; }
row { padding: 14px; border-bottom: 1px solid #26374a; }
row:selected { background-color: #204454; color: #ffffff; }
textview, textview text { background-color: #101822; color: #d7e6f2; }
textview { padding: 16px; }
'''


class Window(Gtk.ApplicationWindow):
    def __init__(self, app, controller, ready_callback=None):
        super().__init__(application=app, title='Surface GRUB三键工具箱')
        self.controller = controller
        self.executor = concurrent.futures.ThreadPoolExecutor(max_workers=1)
        self.busy = False
        self.current = None
        self.selected_version = None
        self.closing = False
        self.ready_callback = ready_callback
        self.set_default_size(1000, 720)
        self.set_position(Gtk.WindowPosition.CENTER)
        header = Gtk.HeaderBar(title='Surface GRUB三键工具箱', show_close_button=True)
        self.set_titlebar(header)
        refresh = Gtk.Button.new_from_icon_name('view-refresh-symbolic', Gtk.IconSize.BUTTON)
        refresh.set_tooltip_text('刷新状态')
        refresh.connect('clicked', lambda *_: self.action('refresh'))
        header.pack_end(refresh)
        self.spinner = Gtk.Spinner()
        header.pack_end(self.spinner)
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16)
        root.set_border_width(20)
        self.add(root)
        panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        panel.get_style_context().add_class('surface-panel')
        root.pack_start(panel, False, False, 0)
        self.status = self.label('正在读取日常菜单状态…', 'surface-status')
        self.version = self.label('')
        self.integrity = self.label('')
        self.acceptance = self.label('', 'surface-hint')
        for label in [self.status, self.version, self.integrity, self.acceptance]:
            panel.pack_start(label, False, False, 0)
        self.message = self.label('音量＋／－选择，短按电源并松开确认。双音量按住返回。', 'surface-hint')
        root.pack_start(self.message, False, False, 0)
        self.buttons = {}
        actions = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE)
        actions.set_max_children_per_line(6)
        actions.set_min_children_per_line(3)
        actions.set_column_spacing(12)
        actions.set_row_spacing(12)
        for action, title, icon in [('check', '检查并收集', 'emblem-ok-symbolic'),
                                    ('enable', '启用菜单', 'media-playback-start-symbolic'),
                                    ('disable', '停用菜单', 'media-playback-pause-symbolic'),
                                    ('rollback', '回退上一版', 'edit-undo-symbolic'),
                                    ('repair', '修复入口', 'preferences-system-symbolic'),
                                    ('steps', '操作说明', 'help-browser-symbolic')]:
            button = Gtk.Button()
            content = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
            content.set_halign(Gtk.Align.CENTER)
            content.pack_start(Gtk.Image.new_from_icon_name(icon, Gtk.IconSize.BUTTON), False, False, 0)
            content.pack_start(Gtk.Label(label=title), False, False, 0)
            button.add(content)
            button.get_accessible().set_name(title)
            button.get_style_context().add_class('surface-action')
            if action == 'check':
                button.get_style_context().add_class('suggested-action')
            button.connect('clicked', lambda _, code=action: self.action(code))
            actions.add(button)
            self.buttons[action] = button
        root.pack_start(actions, False, False, 0)
        self.notebook = Gtk.Notebook()
        root.pack_start(self.notebook, True, True, 0)
        self.versions = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE)
        self.versions.connect('row-selected', self.version_selected)
        version_page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        version_page.pack_start(self.scroller(self.versions), True, True, 0)
        self.use_version = Gtk.Button(label='启用所选版本')
        self.use_version.get_style_context().add_class('surface-action')
        self.use_version.connect('clicked', self.use_selected)
        self.use_version.set_sensitive(False)
        version_page.pack_start(self.use_version, False, False, 0)
        self.notebook.append_page(version_page, Gtk.Label(label='保留版本'))
        log_page = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        self.logs = Gtk.ListBox(selection_mode=Gtk.SelectionMode.SINGLE)
        self.logs.connect('row-selected', self.log_selected)
        self.log_text = Gtk.TextView(editable=False, cursor_visible=False, wrap_mode=Gtk.WrapMode.WORD_CHAR)
        self.log_text.set_monospace(True)
        log_page.pack1(self.scroller(self.logs), False, False)
        log_page.pack2(self.scroller(self.log_text), True, False)
        self.notebook.append_page(log_page, Gtk.Label(label='启动日志'))
        steps_page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        self.steps_text = Gtk.TextView(editable=False, cursor_visible=False, wrap_mode=Gtk.WrapMode.WORD_CHAR)
        steps_page.pack_start(self.scroller(self.steps_text), True, True, 0)
        self.notebook.append_page(steps_page, Gtk.Label(label='使用说明'))
        history = Gtk.Button(label='查看已验收的 M1 历史说明')
        history.connect('clicked', lambda *_: self.action('history'))
        steps_page.pack_start(history, False, False, 0)
        self.history_button = history
        self.details = Gtk.TextView(editable=False, cursor_visible=False, wrap_mode=Gtk.WrapMode.WORD_CHAR)
        self.details.set_monospace(True)
        self.notebook.append_page(self.scroller(self.details), Gtk.Label(label='检查详情'))
        self.action('refresh')
        self.last_index_mtime = (self.controller.base / 'releases.json').stat().st_mtime_ns
        GLib.timeout_add_seconds(3, self.refresh_if_changed)
        self.connect('delete-event', lambda *_: self.busy)
        self.connect('destroy', self.closed)

    def closed(self, *_):
        self.closing = True
        self.executor.shutdown(wait=False)

    def refresh_if_changed(self):
        if self.closing:
            return False
        try:
            mtime = (self.controller.base / 'releases.json').stat().st_mtime_ns
            if mtime != self.last_index_mtime and not self.busy:
                self.last_index_mtime = mtime
                self.action('refresh')
        except OSError:
            pass
        return True

    @staticmethod
    def label(text, css=None):
        label = Gtk.Label(label=text, xalign=0, selectable=True, wrap=True)
        label.set_max_width_chars(95)
        if css:
            label.get_style_context().add_class(css)
        return label

    @staticmethod
    def scroller(child):
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        scroll.add(child)
        return scroll

    def set_busy(self, value):
        self.busy = value
        for button in self.buttons.values():
            button.set_sensitive(not value)
        self.use_version.set_sensitive(not value and bool(self.selected_version))
        self.history_button.set_sensitive(not value)
        if value:
            self.spinner.start()
        else:
            self.spinner.stop()

    def action(self, action, label=None):
        if self.busy:
            return
        self.set_busy(True)
        self.message.set_text({'refresh': '正在读取版本与文件状态…', 'check': '正在检查并只读收集日志…',
                               'enable': '正在启用所选日常版本…', 'disable': '正在停用日常三键入口…',
                               'rollback': '正在回退日常版本…', 'repair': '正在修复用户入口…',
                               'steps': '正在读取操作说明…', 'logs': '正在收集日志…'}.get(action, '正在处理…'))
        future = self.executor.submit(self.controller.run, action, label)
        future.add_done_callback(lambda result: GLib.idle_add(self.finished, action, result))

    def finished(self, action, future):
        if self.closing:
            return False
        try:
            data, result = future.result()
            self.apply_snapshot(data)
            if isinstance(result, str):
                self.steps_text.get_buffer().set_text(result)
                self.notebook.set_current_page(2)
            elif result:
                self.details.get_buffer().set_text(json.dumps(result, ensure_ascii=False, indent=2))
            if action == 'check':
                count = len(result.get('log_collection', {}).get('new_copies', []))
                self.message.set_text('检查完成，新增日志副本 ' + str(count) + ' 份。')
            elif action in ['enable', 'disable', 'rollback', 'repair']:
                self.message.set_text('已完成，改前入口保存在：' + result['backup'])
            elif action == 'refresh':
                self.message.set_text('状态已更新。操作在下次正常启动生效。')
            elif action in ['steps', 'history']:
                self.message.set_text('操作说明已打开。已通过的取键步骤无需重复。')
            self.set_busy(False)
            if self.ready_callback:
                callback, self.ready_callback = self.ready_callback, None
                callback(self)
        except Exception as error:
            self.set_busy(False)
            self.message.set_text('操作未完成：' + str(error))
            self.details.get_buffer().set_text(traceback.format_exc())
            self.integrity.set_text('读取或检查失败，可查看检查详情；原文件和旧版本保留。')
            if self.ready_callback:
                callback, self.ready_callback = self.ready_callback, None
                callback(self)
        return False

    def apply_snapshot(self, data):
        self.current = data
        state = data['state']
        self.status.set_text('日常三键菜单：' + ('已启用' if state['enabled'] else '已停用'))
        self.version.set_text('当前版本：' + state['release'])
        check = data['check']
        self.integrity.set_text('文件检查：' + ('通过' if check['integrity_passed'] else '需要处理'))
        self.acceptance.set_text(check.get('menu_physical_acceptance', ''))
        self.details.get_buffer().set_text(json.dumps(check, ensure_ascii=False, indent=2))
        for child in self.versions.get_children():
            self.versions.remove(child)
        for item in data['versions']:
            row = Gtk.ListBoxRow()
            row.item = item
            box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=5)
            title = ('● 当前  ' if item['active'] else '') + item['label']
            box.pack_start(self.label(title), False, False, 0)
            box.pack_start(self.label(item.get('theme', '') + ' · ' + ('文件完整' if item['valid'] else '文件异常'),
                                      'surface-hint'), False, False, 0)
            box.pack_start(self.label(item.get('blocked') or item.get('acceptance', ''), 'surface-hint'), False, False, 0)
            row.add(box)
            self.versions.add(row)
        self.selected_version = None
        self.use_version.set_sensitive(False)
        for child in self.logs.get_children():
            self.logs.remove(child)
        for item in data['logs']:
            row = Gtk.ListBoxRow()
            row.log_path = item['path']
            title = item['name'].split('-', 1)[0] + ' · ' + str(item['bytes']) + ' 字节'
            label = self.label(title)
            label.set_tooltip_text(item['name'])
            row.add(label)
            self.logs.add(row)
        self.versions.show_all()
        self.logs.show_all()
        self.buttons['enable'].set_tooltip_text('启用当前保留版本，在下次正常启动生效')
        self.buttons['disable'].set_tooltip_text('停用用户三键入口，下一次使用原系统菜单，所有文件保留')
        self.buttons['repair'].set_tooltip_text('核验所选映像后重建用户入口，改前指针自动备份')
        self.buttons['rollback'].set_tooltip_text('回到：' + state.get('previous_release', '')
                                                 if state.get('previous_release') else '没有上一版时停用入口')

    def version_selected(self, _, row):
        if row and row.item['valid'] and not row.item.get('blocked'):
            self.selected_version = row.item['label']
        else:
            self.selected_version = None
        self.use_version.set_sensitive(bool(self.selected_version) and not self.busy)

    def use_selected(self, *_):
        if self.selected_version:
            self.action('enable', self.selected_version)

    def log_selected(self, _, row):
        if not row:
            return
        try:
            self.log_text.get_buffer().set_text(self.controller.read_log(row.log_path))
        except Exception as error:
            self.log_text.get_buffer().set_text(str(error))


class Application(Gtk.Application):
    def __init__(self, controller, ready_callback=None, app_id='local.surface.grub.toolbox'):
        super().__init__(application_id=app_id, flags=Gio.ApplicationFlags.FLAGS_NONE)
        self.controller = controller
        self.ready_callback = ready_callback
        self.window = None

    def do_startup(self):
        Gtk.Application.do_startup(self)
        Gtk.Settings.get_default().set_property('gtk-application-prefer-dark-theme', True)
        provider = Gtk.CssProvider()
        provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), provider,
                                                Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    def do_activate(self):
        if self.window is None or self.window.closing:
            self.window = Window(self, self.controller, self.ready_callback)
        self.window.show_all()
        self.window.present()


def main():
    # Match the existing desktop entry/pinned Surface toolbox, not Python's
    # script basename. GtkApplication still owns one process and one window.
    GLib.set_prgname('surface-grub-keys')
    GLib.set_application_name('Surface GRUB三键工具箱')
    Gdk.set_program_class('surface-grub-keys')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, default=Path.home() / '.local/share/surface-grub-keys')
    args = parser.parse_args()
    if os.geteuid() == 0:
        print('请从桌面用户打开工具箱。', file=sys.stderr)
        return 1
    return Application(Toolbox(args.base)).run([sys.argv[0]])


if __name__ == '__main__':
    sys.exit(main())
