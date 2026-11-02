# 实测与公开验证范围

## 最新原设备GRUB实测

2026-11-02，Surface Pro 8的GM0018.LOG明确绑定a844日常版，EFI SHA256为
a8441f53a3b415009622b695566433cdaa3eb5a5da456e79d35041c5fdb81445。
用户报告：“刚才已测试电源键、音量键、键盘回车、Esc，全测了，而且刚才没有卡顿。”

- 实体音量向上8次、向下9次、短电源松开确认2次和双音量返回1次均有独立事件交付。
- 回车与Esc来自原固件屏幕键盘叶路由，均有明确记录；不混同外接实体键盘来源。
- 高级子菜单返回和最后Ubuntu启动交接正常，脚本错误0、loader1、HANDOFF/END完整。
- 原OSK模式、自刷新、触控producer、主题和居中左对齐布局保持。

首帧614ms、恢复采样637ms；首帧后23ms、15秒初显后15ms已采样，首次EFI叶读键4ms。
这些从D1入口日志计时，不包括父shim/原GRUB链载或完整面板scanout。
用户一次未感到卡顿不保证每种冷启动或Windows后重启都没有等待。
日志仍有1063ms运行采样间隔，细分在1048ms合作采样外层；原OSK可见时子菜单重绘也较慢。
尚未确定更具体的固件调用，不能将这些记录删除或写成全部零阻塞。

当前公共源码同步到该a844版；公开构建使用安装者UUID及签名，产物需要自己的验收。
原设备保留当前a844、直接前版8355、首次原OSK成功06df及手动M1，并已完整备份。
旧候选二进制清理与原始日志、源码和失败证据的保留分开处理。

## Linux电源键证据保持原范围

物理短按后PowerSaveMode=3、锁定为真、网络connected及SSH继续运行的原记录使用
7.2.12-1-surface；后续设备已更新内核，不把旧记录改写成新内核独立验收。
背光恢复与唤醒的维护信号检查单独记录，Linux模块源码与验收未在本次GRUB同步中修改。

## 软件与公开构建

原设备a844已通过实际core/module ASan/UBSan、签名及同SHA隔离OVMF普通键盘、
签kernel/initrd到/init、unsigned拒绝、EFI失败返回、recordfail和完整日志。
OVMF没有Surface原OSK和真实GPIO，不代表公共重新构建后的硬件验收。

tools/check.py只在临时目录检查Python、安装/恢复和版本完整性，不读取实体按键。
prepare-source.py使用固定Ubuntu和Surface补丁；来源与软件检查结果见validation.json。
首次旧公开基线的完整编译记录继续留为历史，不拿它当当前新源码的构建通过。

## 本次源码同步软件核验

7项离线Python/安装恢复检查通过，固定Ubuntu及当前Surface补丁应用通过。
公开overlay与原a844已构建源码逐字匹配；dispatch、writer、console、GOP、生命周期5组ASan/UBSan通过。
GOP宿主fixture已有回调类型warning原样保留；本次未重新生成待部署镜像，也未重复实机验收。
