# Polaris 参考资料

本文件统一维护参考项目；源码分支不重复存放本参考索引。
上游资料说明来源和设计，不代表本项目已完成移植、获得完整硬件支持或通过实机验证。

## Renegade Project / SDM845 UEFI

- [Renegade Project](https://github.com/edk2-porting)：高通平台 UEFI 与 Windows on ARM 相关项目。
- [edk2-sdm845](https://github.com/edk2-porting/edk2-sdm845)：Polaris 的 SDM845 平台、ACPI、UEFI 驱动、BootShim 和启动封装参考。
- 本轮基线：[e1952621f419f8db60ed28271264e1b5184c571d](https://github.com/edk2-porting/edk2-sdm845/tree/e1952621f419f8db60ed28271264e1b5184c571d)。
- 基线所用 EDK II：[e7aac7fc137e247edad22f7ee53b9a1fba227397](https://github.com/tianocore/edk2/tree/e7aac7fc137e247edad22f7ee53b9a1fba227397)。
- [Renegade 文档源码](https://github.com/edk2-porting/renegade-project.org)、[多系统说明](https://github.com/edk2-porting/renegade-project.org/blob/master/en/multiboot.md)：作为历史设计参考，使用前复核目标设备和版本。
- [SimpleInit](https://github.com/BigfootACA/simple-init)：Renegade 基线引导菜单；不等于 PocketJS 公共运行时已经接入。

Windows EFI 入口用于加载已有的 ARM64 Windows Boot Manager，不包含微软系统文件或完整 Windows 驱动包。
Android 冷重启入口与直接解析 Android boot image 是两种不同实现；本轮先采用原生系统冷重启路径。

## postmarketOS / Linux

- [pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports)：设备信息、SDM845 内核打包、配置片段和固件需求参考。
- 本轮读取提交：`d6ca1911e1b1edb03233efa55477bb21badf1516`。
- 设备目录：`device/archived/device-xiaomi-polaris`、`device/archived/firmware-xiaomi-polaris`。它们位于 archived，不能据目录存在宣称当前官方支持。
- 内核配方：`device/community/linux-postmarketos-qcom-sdm845/APKBUILD`，本轮记录 `pkgver=7.1_rc1`、标签 `sdm845-7.1-rc1-r0`。
- [SDM845 mainline Linux](https://gitlab.com/sdm845-mainline/linux)：直接使用上游内核，不另开镜像内核仓库。
- 固定标签：[`sdm845-7.1-rc1-r0`](https://gitlab.com/sdm845-mainline/linux/-/tree/sdm845-7.1-rc1-r0)。
- 固定提交：`85f1df2a4ec71d7a91dd95a7a49f889d1595ffa8`；Makefile 版本为 `7.1.0-rc1`。
- 板级 DTS：`arch/arm64/boot/dts/qcom/sdm845-xiaomi-polaris.dts`。
- [Linux EFI stub 文档](https://docs.kernel.org/admin-guide/efi-stub.html)：EFI-stub 内核、命令行与 initrd 交接参考。LinuxBoot 应用本身不包含内核、DTB 或 rootfs。

7.1-rc1 与历史 6.12 内核、模块、配置和镜像不能混用。postmarketOS 包装参数也不能直接替代 Polaris UEFI 封装参数。

## 公共组件与目录边界

- [pj4edk2](https://github.com/Yeamika/pj4edk2)：PocketJS 公共 ABI、运行时与默认 UI。
- [w202ds-linux](https://github.com/Yeamika/w202ds-linux)：仅参考 Linux / boot 分支及硬件机制分层；W202DS 的 UMS9620 驱动、LK 返回接口和分区偏移不适用于 Polaris。
- [UEFI Specification](https://uefi.org/specifications)：LoadImage、StartImage、Loaded Image、配置表与 ResetSystem 等接口定义。

待移植内容应记录实际源码提交、编译输入和产物哈希；“已构建”与“已在设备上验证”分别记录。
