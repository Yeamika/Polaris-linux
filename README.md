# Polaris boot

Xiaomi MIX 2S / Polaris / SDM845 板级启动工程，对应 `Polaris-linux` 仓库的 `boot` 分支。

| 目录 | 职责 |
| --- | --- |
| `pocketjs4edk2_uefi/` | 参照 W202DS 的组合层边界：板级适配、EFI 加载与最终封装 |
| `components/` | SDM845 EDK2 平台和公共 PocketJS 组件的依赖声明 |
| `output/` | 唯一默认构建输出，Git 忽略 |

当前是目录骨架，尚未迁入旧 EDK2 本地修改，未实现新 PocketJS ABI 或可用启动镜像。
Polaris 使用 SDM845 平台，不复制 W202DS 的 UMS9620 平台、LK 容器或分区偏移。
boot/system 的原生恢复资料和私有镜像不属于本项目；不包含刷机命令。
