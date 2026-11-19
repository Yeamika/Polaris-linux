# Polaris boot

Xiaomi MIX 2S / Polaris / SDM845 板级启动工程，对应 `Polaris-linux` 的 `boot` 分支。

| 目录 | 职责 |
| --- | --- |
| `pocketjs4edk2_uefi/` | 板级构建、EFI 加载、USB 恢复与最终封装 |
| `components/` | 公共 PocketJS 协议头依赖声明 |
| `output/` | 唯一默认构建输出，Git 忽略 |

目前可本地构建 Renegade UEFI 基线与 Windows、Android、LinuxBoot、Recovery EFI。
恢复服务已实现查询、下载和 AArch64 EFI 的 RAM 启动代码；分区刷写、RAM 读取、
硬件看门狗和 Android boot 镜像临时启动仍待完成。

构建方式见 [构建说明](pocketjs4edk2_uefi/README.md)，恢复功能及设备验证步骤见
[恢复机制](pocketjs4edk2_uefi/RECOVERY.md)。构建通过不代表设备验证通过。
Polaris 使用 SDM845 平台，不复制 W202DS 的 UMS9620 平台、LK 容器或分区偏移。
