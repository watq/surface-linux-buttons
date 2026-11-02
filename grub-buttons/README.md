# GRUB 三键菜单与图形工具箱

[返回项目说明](../README.md) · [English](../docs/en/README.md#grub-volume-and-power-buttons)

本目录公开 Surface Pro 8 已验收的 D1 扩展：音量选择、电源确认、原系统菜单导入、左对齐居中的菜单块、大字号主题和 GTK GUI。GPIO 资源契约来自这一台 Surface 的 ACPI 与控制器布局。资源不匹配时拒绝启用三键，不能直接迁移到 ROG、其他品牌或所有 Surface。

## 构建环境

使用 **x86_64 Ubuntu Linux** 构建，准备编译依赖：

```bash
sudo apt install build-essential autoconf automake bison flex gettext autopoint gawk python3 pkg-config grub-common sbsigntool
python3 grub-buttons/prepare-source.py build/source
cd build/source/grub-2.12
env AUTOPOINT=true ./autogen.sh
cd ../../..
mkdir -p build/grub
cd build/grub
env AWK=gawk ../source/grub-2.12/configure --with-platform=efi --target=x86_64 --disable-werror --disable-grub-mkfont --disable-device-mapper --disable-efiemu --disable-nls --disable-grub-mount --disable-libzfs
make -j2
cd ../..
```

`prepare-source.py` 校验固定 GRUB 2.12 归档，应用 Ubuntu `2.12-1ubuntu7.3` 补丁，再应用 Surface 核心补丁和相应源码覆盖。不执行安装，不修改系统引导器。`AUTOPOINT=true` 使用归档已有 gettext 文件；禁用 NLS 的构建仍可使用打包的中文字体。

## 生成独立 EFI

先确认 `/boot/grub/grub.cfg` 所在文件系统的 UUID；独立 `/boot` 分区布局需要重新设计路径，当前配方按 `/boot` 在根文件系统内编写：

```bash
findmnt -no UUID -T /boot/grub/grub.cfg
python3 grub-buttons/build.py --grub-build build/grub --root-uuid 实际UUID --output dist/local-d1 --key /本机签名目录/owner.key --cert /本机签名目录/owner.pem
```

Secure Boot 需要签名证书已被目标启动链信任。证书登记由使用者在本机完成，参考 [linux-surface 安装说明](https://github.com/linux-surface/linux-surface/wiki/Installation-and-Setup)。本工具不会自动登记 MOK、禁用 Secure Boot 或写入固件启动顺序。

可以省略 `--key` 和 `--cert` 生成未签名开发镜像；未签名镜像不适用于启用 Secure Boot 的日常启动。签名文件、私钥和生成的 EFI 不纳入 Git 跟踪。

本次源码构建出的版本标为“新本地构建，待实机验收”，不会沿用作者机器的验收标签。

## 安装 GUI

桌面用户操作，先安装 GTK 依赖，再安装全新的用户目录：

```bash
sudo apt install python3-gi gir1.2-gtk-3.0
python3 grub-buttons/install-toolbox.py --release dist/local-d1
python3 ~/.local/share/surface-grub-keys/gui.py
```

安装后菜单入口初始停用。GUI 使用单实例 GTK Application，检查版本、启用/停用、恢复版本、查看步骤和日志；由桌面正常启动，不用 root 跑 GUI。

安装器拒绝覆盖已有 `~/.local/share/surface-grub-keys`。现有 Surface 部署保留原目录与历史版本；不要直接跑此安装器覆盖它。若只是检查新公开构建，用另一个纯 ASCII 的 `--base` 目录。

## 先手动进入，再启用日常入口

确认目标文件系统可被当前 GRUB 读取，连接键盘，在原菜单按 `c`，按实际位置调整设备根后执行：

```text
search --no-floppy --fs-uuid --set=root 实际UUID
chainloader /home/实际用户/.local/share/surface-grub-keys/releases/实际版本/grub-daily.efi
boot
```

检查三个实体按键、普通键盘、当前系统启动和返回；不要将软件模拟结果当作本机启动通过。

通过后，从 Linux 显式添加扩展入口：

```bash
sudo python3 grub-buttons/install-toolbox.py --base /home/实际用户/.local/share/surface-grub-keys --install-hook
python3 ~/.local/share/surface-grub-keys/tool.py daily-enable
```

安装器要求原 `grub.cfg` 已 source `custom.cfg`，只新建该扩展点，不重新生成菜单、不改 EFI 或 BootOrder。若 `custom.cfg` 已存在则拒绝覆盖；请备份该文件，把 `tool.daily_hook_text(base)` 生成的内容与原扩展人工合并并用 `grub-script-check` 检查。用户名和路径请使用实际 ASCII 值。

## 按键与菜单

| 操作 | 按键 |
| :-- | :-- |
| 上一项 | 音量＋ |
| 下一项 | 音量－ |
| 确认当前项 | 短按电源并松开 |
| 子菜单返回上一层 | 双音量约半秒或 Esc |
| 主菜单退出独立镜像 | 双音量约半秒或 Esc |

普通方向键与 Enter 可继续使用。原固件屏幕键盘保留图标、自刷新和模式，原设备已明确通过屏幕键盘回车与Esc；不替换键盘或将其关闭来隐藏延迟。系统提供的超时值继续沿用，未指定时默认 15 秒。公共配方读取当前系统菜单，不替目标机器卸载内核、memtest 或其他软件。

## 恢复与日志

GUI “停用”或如下命令会把日常指针改为停用，回到原系统菜单：

```bash
python3 ~/.local/share/surface-grub-keys/tool.py daily-disable
python3 ~/.local/share/surface-grub-keys/tool.py daily-rollback
```

每次指针变更都会保留改前副本；没有上一版时恢复操作直接停用。不要删掉整个工具箱目录来代替版本恢复。

如果 Linux 启动前就遇到错误，连接键盘从原 GRUB 进入正常系统，再停用。公共安装流程保留原 EFI；独立镜像路径缺失、加载失败或退出会继续原启动链。

GRUB 日志写入 EFI 分区 `SP8-KEYDIAG/GMxxxx.LOG` 的新文件。工具箱只收集副本到用户 `logs/boot/`，保留原文件；分享日志前检查文件系统 UUID、系统路径等本机信息。

## 已知界限

- 本文源码对应2026-11-02 a844；原设备GM18三键/原OSK回车Esc通过且用户未感到卡顿，日志仍有约1.06秒运行采样间隔及键盘可见后较慢重绘，不泛化为所有启动零等待。
- GPIO 模块校验 ACPI 身份、关键 AML 字节、NVS、映射、ownership 和输入配置，只读取合法资源，失败时停止采样。它不提供任意品牌的通用按键检测。
- 48/36/24 为不同显示模式的菜单字体档位；布局为多行整体居中、各行文字左对齐，下方说明独立。
- 内核更新时读取当前 `grub.cfg`，但未来 GRUB/shim/SBAT、固件和签名策略变化仍可能影响兼容性，须重新核验。


## 当前版本日志和回退节点

当前a844记录首帧/采样、实际按键来源、原屏幕键盘/GOP、系统交接与END；INPUT_COMPONENT、INPUT_HOOKS及LOG IO_SUMMARY用于区分合作采样、各终端和I/O实际时间。
常规元数据4096字节合并，关键按键/错误/交接立即刷盘；正常轮询约100ms检查尾段，突然断电可能缺最后未刷的常规元数据。
原设备保留a844、直接前版8355、原OSK首次通过06df及手动M1四个关键节点，已完整备份。新用户按自己的验收与回退需要保留节点；不要删除当前或指针仍引用的版本。
