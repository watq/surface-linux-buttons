# Linux 电源键：锁屏、息屏、保持在线

[返回项目说明](../README.md) · [English](../docs/en/README.md#linux-power-button)

适用已验证的 Surface Pro 8、Ubuntu 24.04、GNOME Wayland。服务依赖 Linux 的 `soc_button_array` 输入设备、`intel_backlight`、`/dev/uinput`、logind、用户会话 D-Bus 与 Mutter。

## 安装前检查

在目标设备正常登录 GNOME，确认当前桌面用户：

```bash
id -un
loginctl list-sessions
ls /sys/class/backlight/intel_backlight
ls /dev/uinput
```

不能在只有 SSH、没有 GNOME 登录的环境中完成显示控制。不要将服务用户设成 root。

服务自动寻找系统路径含 `MSHW0040`、仅有 `KEY_POWER=116` 的事件设备。没有找到就退出，由 systemd 稍后重试；不会抓取整块键盘来凑数。默认背光路径固定为 `intel_backlight`，其他硬件需要修改和独立验证。

## 安装

从仓库根目录操作，将 `desktop` 换为实际 GNOME 用户名：

```bash
sudo apt install python3-gi
python3 power-button/install.py --user desktop
sudo python3 power-button/install.py --user desktop --apply
systemctl status surface-screen-key.service
journalctl -u surface-screen-key.service -n 50
```

安装器先保存原文件、原启用状态、原运行状态以及三项 GNOME 设置，再写入 `/usr/local/libexec` 和 `/etc/systemd`。备份位于 `/var/lib/surface-screen-key/backups/时间戳/`，输出的 `receipt.json` 用于恢复。原件仍在备份中，系统包文件不被替换。

设置变更：

| 位置 | 项目 | 安装后的值 |
| :-- | :-- | :-- |
| GNOME power | power-button-action | nothing |
| GNOME power | sleep-inactive-ac-type | nothing |
| GNOME power | sleep-inactive-battery-type | nothing |
| logind | HandlePowerKey、IdleAction | ignore |

安装器向 logind 发送 HUP 重载配置，避免重启登录会话。其他盖子和低电量策略没有被一并禁用；如果它们在目标设备上触发休眠，息屏保持连接的条件也就不成立。

## 物理使用与验收

1. 在亮屏桌面短按电源：锁定确认后立即熄灭背光，随后关闭显示管线。
2. 再短按电源：恢复显示，停留在锁屏界面，按正常方式解锁。
3. 在另一台设备保持 SSH 或后台任务，确认屏幕关闭时连接仍在。

如果锁屏请求失败或 4.5 秒内没有确认，显示器保持亮着。服务采用约 250ms 防重复按下；自动重复事件不触发动作。硬件长按强制关机没有改写。

`SIGUSR1` 是维护入口，复用同一动作但不代表实体电源事件已经通过；不要在无人看守时拿它作为安装检查。

## 恢复

```bash
sudo python3 power-button/install.py --restore /var/lib/surface-screen-key/backups/时间戳/receipt.json --apply
```

恢复器先核对安装后的文件哈希和原备份哈希，再停止服务、恢复原文件、GNOME 设置和旧服务状态。若后来有管理员另改文件，先自行核对差异，脚本会拒绝覆盖。备份不会删除。

若显示异常，可通过 SSH 停止服务。`ExecStopPost` 会恢复记录的背光亮度；如果需要明确恢复，也可执行：

```bash
sudo systemctl stop surface-screen-key.service
sudo python3 /usr/local/libexec/surface-screen-key.py --restore-backlight
```

这条恢复命令只恢复背光数值；如果 Mutter 仍在关屏模式，使用键盘/触摸唤醒正常锁屏，或先按正常桌面故障处理。不要把恢复背光理解成解锁。

## 软件实现

守护进程只依赖 Python 标准库与 PyGObject，不需要 `python3-evdev`。读取本机 ABI 的 `struct input_event`，只抓取专用电源节点；启动时取得受限描述符后降到选定桌面用户。唤醒虚拟键盘只发送 Shift，不输入密码或文本。

本文说明基于已运行版本；公开版只将机器用户名改为安装参数。Linux 显示控制和 GRUB 启动阶段 GPIO 完全独立。

