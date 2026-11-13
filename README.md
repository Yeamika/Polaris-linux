# Polaris Linux 7.1-rc1

Xiaomi MIX 2S / Polaris / SDM845 的 Linux 板级工程。目录职责参照 W202DS Linux；
引导固件在同仓库的 `boot` 分支，发行版和服务启用策略由 REIMUKS 管理。

| 目录 | 职责 |
| --- | --- |
| `kernel/` | 直接固定上游 SDM845 Linux 子模块，不另建内核仓库 |
| `Devices/` | 按硬件维护板级增量和用户态机制；当前未迁入旧驱动 |
| `devicetree/` | 板级 DTS 入口索引，复用上游 Polaris DTS |
| `config/` | 7.1-rc1 配置计划和片段，尚未验证构建 |
| `scripts/` | 源码身份和完整性检查 |
| `output/` | 唯一默认构建输出，Git 忽略 |

内核来源、提交和初始化方式见 [KERNEL-GIT.md](KERNEL-GIT.md)。当前只完成目录与来源固定，
没有生成匹配的 Image、DTB 或模块，也没有 7.1-rc1 实机验收。旧 6.12 模块与镜像不得混用。
本地若为稀疏检出，需显式补齐后再准备配置。硬件机制、发行版策略与启动封装分别维护，
不直接复制 W202DS 驱动、服务或 UMS9620 镜像布局。
