# Surface Linux 实体按键工具箱

**简体中文** | [English](docs/en/README.md)

让 Surface Pro 8 脱离键盘也能选择启动系统，并在 Linux 中短按电源键锁屏、息屏，保持后台任务和网络连接。两个模块可以分别使用。

本仓库是 [linux-surface](https://github.com/linux-surface/linux-surface) 的配套用户空间工具和独立 GRUB 扩展。Linux 内核安装与设备支持请参考该项目；本仓库独立维护，尚未合并到上游。

## 当前状态（2026-11-02）

| 模块 | Surface Pro 8 实测结果 | 当前限制 |
| :-- | :-- | :-- |
| Linux 电源键 | 短按锁屏后息屏；再按唤醒；息屏期间网络和 SSH 保持连接 | GNOME Wayland、MSHW0040 专用电源事件、Intel 背光 |
| GRUB 三键 | 音量＋/－选择，电源确认；用户反馈其他验收通过 | 启动仍有短暂停顿，正在继续优化 |
| GRUB 图形工具箱 | 深色 GTK 窗口、版本检查、启用/停用、日志、恢复 | 新机器需要本地构建和签名 |

公开 GRUB 源码取自已验收的 **2026-10-30 D1** 版本；后续卡顿修订未混入。新机器重新构建出的 EFI 需要独立验证。详细版本和证据范围见 [验证记录](docs/validation.md)。

## 特性

- **两段独立使用**：Linux 电源键服务位于 `power-button/`，GRUB 按键和 GUI 位于 `grub-buttons/`。
- **锁定后才息屏**：先请求桌面锁定，收到 `LockedHint` 确认后关闭背光与显示管线；锁定失败则保持亮屏。
- **息屏保持运行**：关闭的是显示器，系统保持唤醒。后台任务、网络、SSH 可继续；不会声称休眠也能保持网络。
- **三键启动菜单**：音量＋向上、音量－向下、短按电源并松开确认；双音量约半秒或 Esc 返回。
- **沿用系统菜单**：每次启动读取当前系统 `grub.cfg`，保留系统启动项和内核路径；菜单整体居中、文字左对齐、说明放在下方。
- **独立版本和恢复**：GRUB EFI 落在用户目录，原系统 EFI 保留；Linux 服务安装前备份原文件与 GNOME 设置。

## 兼容性

| 设备/环境 | Linux 电源键 | GRUB 三键 | 说明 |
| :-- | :-- | :-- | :-- |
| **Surface Pro 8 / Ubuntu 24.04 / GNOME Wayland** | 已实测 | 已实测 | linux-surface 内核；本项目依据 |
| 其他 Surface | 尚未验证 | 尚未验证 | Linux 事件节点、背光、ACPI 与 GPIO 均须核实 |
| ROG 幻 X 2023 / RTX 4060 等其他二合一 | 尚未验证 | 不可直接套用 | 相同外形和 Linux 键码不能证明启动阶段 GPIO 相同 |
| KDE、XFCE、X11 | 当前实现不支持 | 与 Linux 桌面无关 | 息屏服务调用 GNOME Mutter 接口 |

## 第一块：Linux 电源键锁屏与息屏

### 问题与实现

默认短按电源键可能触发 suspend，导致远程任务或热点断开。本模块消费 **MSHW0040 下只含 KEY_POWER（116）的专用事件设备**，避免把音量、触摸或普通键盘一起抓取。

服务以管理员身份打开少量设备描述符，然后降权到桌面用户，复用用户 D-Bus 与系统 logind。按下时立即响应，忽略重复事件；先锁屏、确认锁定，再熄灭背光和调用 Mutter `PowerSaveMode=3`。唤醒时恢复显示并发送不输入文字的 Shift 事件，仍需正常解锁。

### 安装与恢复

先按 [Linux 模块说明](power-button/README.md)检查环境。使用当前已登录 GNOME 的实际用户名替换 `desktop`：

```bash
sudo apt install python3-gi
python3 power-button/install.py --user desktop
sudo python3 power-button/install.py --user desktop --apply
```

第一条脚本命令只显示计划；第二条实际安装服务并保存回执。安装会将 GNOME 的电源键操作及 AC/电池空闲休眠设置为 `nothing`，并让 logind 忽略短按电源键和空闲动作。

```bash
systemctl status surface-screen-key.service
journalctl -u surface-screen-key.service -n 50
sudo python3 power-button/install.py --restore /var/lib/surface-screen-key/backups/时间戳/receipt.json --apply
```

恢复保留备份；如果安装后文件另被修改，脚本拒绝覆盖。详细故障恢复见模块说明。

## 第二块：GRUB 三键菜单与图形工具箱

### 为什么 Linux 键码不能直接改 GRUB

Linux 上的音量－/＋/电源码 `114/115/116` 来自内核输入子系统。本机固件的普通 EFI 键盘接口没有提供对应实体按钮事件，仅修改 GRUB 常量不能补出事件。

本模块根据 Surface Pro 8 已核实的 ACPI 资源和 Tiger Lake GPIO 布局，在启动阶段校验资源、映射属性、所有权和输入状态，只采样按键，不修改 GPIO 配置寄存器。遇到不匹配会停止三键采样并保留普通键盘；这也是不能直接套用其他品牌的原因。

### 构建、安装和使用

完整命令见 [GRUB 模块说明](grub-buttons/README.md)。仓库包含原始 GRUB 2.12、Ubuntu 补丁、Surface 源码覆盖与 GUI。

```bash
python3 grub-buttons/prepare-source.py build/source
```

在 x86_64 Linux 上编译后，使用本机文件系统 UUID 构建独立 EFI；Secure Boot 环境使用安装者自己的本地证书签名并登记信任。仓库不分发机器私钥。

图形工具箱由桌面用户安装，**初始停用**。确认 EFI、签名与手动启动后，再加 `custom.cfg` 扩展入口和启用日常菜单。安装器拒绝覆盖现有工具箱与 `custom.cfg`；已有部署请先看恢复说明。

## 已知问题

- GRUB 启动仍可能短暂停顿。剩余时间是否来自固件 EFI 输入、GOP 或其他初始化，须按真实启动日志区分，未宣布完全修复。
- Linux 服务硬件选择固定为 Surface 专用电源设备和 `intel_backlight`；换机不能仅改用户名。
- 息屏保持在线需要系统持续运行；不代表待机功耗与 suspend 相同。
- Linux 维护信号测试和实体按键测试分别记录；软件模拟不当作新机器实机通过。
- 独立 EFI 不会被普通包更新直接替换，但内核、GRUB、shim、SBAT 或固件更新后仍需检查兼容性。
- 移除 memtest 是原机器的用户选择；公共构建沿用目标机器当前菜单，不替他人删除启动项、内核或软件包。

## 仓库内容

```text
power-button/             电源键服务、systemd 模板、可恢复安装器
  README.md
  surface-screen-key.py
  install.py
grub-buttons/             GRUB 补丁、GPIO/菜单源码、主题、构建器、GTK 工具箱
  README.md
  src/
  toolbox/
vendor/                   核验过的 GRUB 2.12 与 Ubuntu 补丁
licenses/                 主题、字体、发行版版权说明
docs/en/README.md         English documentation
docs/validation.md        实测范围及公开验证记录
tools/check.py            离线检查，绝不读取实体按键或修改桌面
```

## 验证

```bash
python3 tools/check.py
```

检查 Python 语法、源码归档校验、安装/恢复、后续修改保护、GRUB 版本完整性与路径边界。CI 同时验证全部补丁可应用；实机启动验收仍须在目标设备完成。

## 上游与相关项目

- [linux-surface/linux-surface](https://github.com/linux-surface/linux-surface)：Surface 内核、安装和设备兼容性。
- [GNU GRUB](https://www.gnu.org/software/grub/)：启动器上游。
- [Ubuntu grub2 source](https://launchpad.net/ubuntu/+source/grub2)：本仓固定发行版补丁的来源。
- [vinceliuice/grub2-themes](https://github.com/vinceliuice/grub2-themes)：Vimix 主题来源。
- [Microsoft Mu OnScreenKeyboard](https://github.com/microsoft/mu_plus)：固件屏幕键盘协议依据。
- [本人的 K90 Linux 桌面仓库](https://github.com/watq/gpu-desktop-for-android-container)：文档与发布结构参考。

## 授权

本仓代码按 **GPL-3.0-or-later** 发布，保留 GRUB/Ubuntu 原版权头。主题按其 GPL 授权，Noto CJK 字体与生成的字体资源按相应 OFL 授权；详见 [第三方授权](licenses/README.md)。
