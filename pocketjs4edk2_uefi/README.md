# Polaris UEFI build

离线编译 Renegade/SDM845 UEFI、Windows/Android/LinuxBoot/Recovery 四个 AArch64 EFI
应用，以及用于 RAM 启动往返验证的 ReturnProbe。默认界面使用 PJ4EDK2，SimpleInit
提供后备菜单和 Android 镜像准备能力。恢复能力和验证边界见 [RECOVERY.md](RECOVERY.md)。

需要初始化的 `edk2-sdm845` 平台及其 EDK II、edk2-platforms、SimpleInit、EFI_Binaries
依赖，AArch64 GNU 工具链，以及已编译的 EDK II BaseTools。
`components/pj4edk2` 与 `components/pj4edk2-linux-efi` 提供共享协议头。

在 boot 仓库根目录运行：

```sh
export EDK_TOOLS_PATH=<initialized-edk2>/BaseTools
python3 pocketjs4edk2_uefi/build.py \
  --platform <initialized-edk2-sdm845> --workdir <build-cache> \
  --entry pocketjs --jobs 8 --android-boot-image <original-android-boot.img>
```

默认固定输出为 `output/`，构建缓存必须与输出和源码分开。
`--stage efi` 只构建独立 EFI；默认 `all` 同时打包固件。
`--entry pocketjs` 启动 PJ4EDK2，并在后台提供 USB fastboot。
`--entry menu` 自动进入菜单，`--entry recovery` 先启动内置 fastboot 恢复服务，
恢复服务返回后进入菜单。平台源码只在私有构建副本内打补丁。

Polaris 继承 SimpleFb 的 1080×2160 竖屏缓冲。PolarisRotatorDxe 通过可选的
`PJ4EDK2_ROTATE_PROTOCOL` 将 PJ4EDK2 的 2160×1080 图像顺时针旋转 90°，
再把更新区域写入现有缓冲。独立 SDM845 Rotator 使用驱动自有的低 4 GiB DMA 缓冲，
不改变 DPU/DSI 的扫描输出。小于 600,000 像素的更新、协议缺失或硬件失败使用 CPU
回退；每个硬件任务仍处理完整图像。进入 EFI 子程序前必须停止 DMA 并验证 SMMU
路由恢复，失败时阻止交接。退出 Boot Services 时也停止硬件，且不修改内存映射。
冷启动时空闲 context 的 CBAR 类型会被固件保留为 S1，写回零不能撤销；交接时仅允许
这一已验证的差异，并要求该 context 保持禁用、无路由引用，其余保存值恢复。
透明混合和栅格化仍由 CPU 完成。渲染器的 ARGB8888 在 ARM64 小端
内存中与 GOP 的 BGRX 字节布局兼容，呈现时不再转换颜色通道。SimpleFb 的缓存清理
限定为 BLT 写入的更新区域，整屏更新合并为一次清理；读取和失败的 BLT 不清理屏幕。
这项改动不改变显示模式或 EFI 子程序的显示交接。

产物包括 `polaris-uefi.fd`、Android boot header v1 的
`polaris-uefi-recovery.img`、`ESP/EFI/*/BOOTAA64.EFI`、`tools/return-probe.efi`
和记录哈希、源码身份及未完成能力的 `firmware-manifest.json`。
镜像文件名中的 recovery 表示封装用途，不代表需要刷写 recovery 分区。

REIMUKS 通过 `build-firmware.py` 调用相同构建器。
`--source-cache` 指向包含已初始化 `edk2-sdm845/` 的目录，或提供板级输入 JSON：

```json
{"schema": 1, "board": "polaris", "platform": "<platform-source>", "entry": "recovery"}
```

输入 JSON 的相对路径以该 JSON 所在目录为基准。构建器不下载依赖，也不连接设备。

LinuxBoot 从同一 ESP 的 `EFI/LinuxBoot/` 读取 `Image.efi`、`polaris.dtb`、
`cmdline.txt` 和必需的 `initrd.img`，通过 EFI stub 和 LoadFile2 启动内核。
Linux EFI 会停止固件 USB/旋转工作、发出 ReadyToBoot，再启动内核。SDM845
初期验证使用 `efi=noruntime,novamap`：`noruntime` 仅禁用内核运行时服务，
不能代替 EFI stub 的 `novamap`。引导器与 EFI stub 的文本最多保存 32 KiB 到
`EFI/LinuxBoot/boot-status.txt`；ExitBootServices 通知不执行文件操作，最后一行
只能证明交接前的进度，不能证明内核已经运行。日志写入失败不阻止正常启动。
交接前使用固件原有的左上角日志网格，与早期 DEBUG 共用光标和字体。
`tools/linux-inspect.efi` 可通过 fastboot 在 RAM 执行，读取 ESP 的 EFI/initramfs
日志后返回菜单。`oem log` 输出快照地址和长度，再用 `tools/debug.py read` 完整
提取。该工具只读文件，导出的数据保留到重启；不需要启动救援系统读取这些日志。
显示交接验证 Polaris 的保留 BGRX 扫描区，在 DT 中添加 `no-map` 和
`write-combine` 映射契约；必须配合 Polaris Linux 的 `POLARIS_BOOT_DISPLAY`
接收端。早期日志继续使用固件画面，MSM DRM 的 aperture 移交会先停止日志写入，
再开始原生显示初始化。两阶段状态由 initramfs 单独报告；编译通过不代表实机接管成功。
Windows EFI 检查平台 ACPI、SoC 信息和保留内存，补齐早期菜单跳过的准备步骤后，
加载已存在的 Windows Boot Manager。需要与支持 Windows 看门狗交接协议的固件
配套部署；调用顺序、恢复和测试范围见 [Windows EFI](efi/Windows/README.md)。

系统卡片与 W202DS 一样显示 ESP 上的 `\\EFI\\<system>\\bootaa64.efi` 路径。
菜单文件位于 `EFI/BOOT/pj4edk/config.json`；已有文件会覆盖固件默认值，部署更新时
应合并菜单条目并保留自定义配置及启动偏好。SimpleInit 后备入口保留在固件 FV 中。

Android 的菜单入口从同一个 ESP 读取 `EFI/Android/boot.img` 与
`EFI/Android/board.dtb`，直接启动该 ROM 的内核和 ramdisk。更换 ROM 时更新这两份文件；
启动程序不依赖 boot 分区里的 Android 内核、镜像校验值或 PMIC 暖复位。
当前支持 Polaris MP / SDM845 v2.1 的 legacy v0/v1 Android 镜像。设备树必须来自同一 ROM
的原生启动，含 ABL 修正、原生命令行和内存保留区。优先捕获 `/sys/firmware/fdt`；
如果只有 `/proc/device-tree` 的归档，可用 `tools/native-dtb.py` 重建设备树，且需要
确认 ABL 未改变匹配 boot DT 的保留表。捕获数据不随源码发布。

默认使用固件内置的原生加载器；也可以在同一个目录放入匹配 ABI 的 `loader.efi`，
独立更新加载器。 `AndroidPrepare.efi` 使用相同路径校验和准备镜像，到内核交接前返回。
原有构建器的 `--android-boot-image` 仅保留作为旧 boot 封装兼容选项；当前 Android
启动程序不读取其 BootShim 标志或镜像绑定。

交接诊断在预留固件映射末页 `0xcffff000` 保留 64 字节阶段记录，并在传给内核的 DT
中保留该页。当前启动中的记录可通过 fastboot 的 RAM 读取功能取得；普通重启实测
会覆盖这一区域，因此不能依靠它恢复上次启动的最后阶段。阶段 `0x31` 为准备退出
Boot Services，`0x40` 为退出成功，`0x50` 为进入汇编，`0x54` 为关闭 MMU 后即将跳入内核。
该记录用于区分交接阶段，不代表内核或 Android 用户空间已启动。

调试命令和构建验证范围见 [DEBUGGER.md](DEBUGGER.md)。

Polaris Linux 的 ESP 启动器现使用原生 ARM64 交接：校验 Image、DTB、
initramfs 与可用内存，退出 Boot Services 后完成缓存/MMU 交接。标准 EFI-stub
路径保留在显式诊断工具中。实机检查点已到达 `/init` 的 EL0 系统调用和 mount
系统调用，尚未取得目标 USB 串口或完成 server 启动。

Linux 屏幕日志消费 pj4edk2 的 BootDisplay 区域；经 fastboot 直接加载诊断 EFI
时没有卡片区域，会使用明确的全屏回退。ESP 日志会报告采用的区域和旋转。
关闭 ESP 文件不再撤销屏幕重定向，EBS 回调也不会恢复固件的旧光标；
只在应用于 Boot Services 内返回时恢复原接口。
