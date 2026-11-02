# 固定 GRUB 构建基线

`grub-2.12.tar.xz` 为项目原设备使用的 Ubuntu grub2 原始源码归档；SHA256 见 `SHA256SUMS`。`ubuntu-patches/`、`ubuntu-changelog.txt` 来自 Ubuntu `grub2-unsigned 2.12-1ubuntu7.3`。

来源：[GNU GRUB](https://www.gnu.org/software/grub/) 和 [Ubuntu grub2 source](https://launchpad.net/ubuntu/+source/grub2)。版权说明见 `ubuntu-copyright.txt` 和归档里的 `COPYING`。

保留该固定版本以复现已验收项目；发行版后续安全更新不会自动回写到这一独立源码。升级基线需重新核验补丁、签名策略与实机启动。

