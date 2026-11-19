# Polaris Linux 7.1-rc1

Xiaomi MIX 2S / Polaris / SDM845 的 Linux 板级工程。目录职责参照 W202DS Linux；
引导固件在同仓库的 `boot` 分支，发行版和服务启用策略由 REIMUKS 管理。

| 目录 | 职责 |
| --- | --- |
| `kernel/` | 直接固定上游 SDM845 Linux 子模块，不另建内核仓库 |
| `Devices/` | 板级增量；包含 EFI 保留画面到 MSM DRM 的日志交接接收端 |
| `devicetree/` | 板级 DTS 入口索引，复用上游 Polaris DTS |
| `config/` | 固定的 7.1-rc1 来源与 desktop 内核配置片段 |
| `patches/kernel/` | 构建时应用于私有源码副本的板级内核修正 |
| `scripts/` | 配置、编译和匹配的内核运行时导出 |
| `output/` | 唯一默认构建输出，Git 忽略 |

内核来源、提交和初始化方式见 [KERNEL-GIT.md](KERNEL-GIT.md)。基线为
`sdm845-7.1-rc1-r0`，带 SDM845 适配；旧 6.12 模块与镜像不得混用。

依赖完整的固定内核子模块、`components/boot-display`、Python 3.11+、GNU make、
完整 AArch64 GCC/binutils、原生编译器、flex、bison、bc、OpenSSL 开发头文件。
本地若为稀疏检出，需先补齐。默认输出固定在 `output/`，源码覆盖层缓存为 `.cache/`。

```sh
python3 scripts/build.py --profile desktop --jobs 8 all
python3 scripts/runtime-export.py --profile desktop
```

导出 `output/runtime-desktop/` 的 Image、DTB、config、模块和身份清单。
REIMUKS 使用这套匹配产物生成 initramfs 与内核运行时包；`desktop/server` rootfs
独立构建，部署时还需安装相同内核运行时。

早期显示通过共享 KernelBoot 的标准 fbcon 窗口，接收 LinuxBoot EFI 验证过的
Polaris BGRX 扫描区，要求 DT `no-map` 和 write-combine 映射，避免缓存别名。
fbcon 在普通 RAM 中负责字体、滚动、旋转和 Linux 标志；Polaris 的 present worker
只将窗口内的改动行复制至 WC 扫描区。启动参数使用
`console=tty1 fbcon=font:TER16x32`，内核启用 FB_SYSMEM_HELPERS、fbcon rotation、
TER16x32 和 Linux CLUT224 标志。80 字节显示协议沿用现有 EFI 发送端。

MSM DRM 通过 aperture 接管时，先禁止新任务、同步注销 fbcon，再等待复制任务结束，
最后释放旧 WC 映射，并保留整页 RAM 快照交给原生驱动。原生 MSM fbdev 使用
`polaris-native-fbcon.h` 复用共享 fbcon 绘制，将背景和日志窗口迁到独立的原生 KMS
缓冲区。native pitch 改变时逐行搬运；后续仅复制窗口内的改动行。Linux 原生控制台
延续描述中的区域与软件旋转，systemd 使用 `systemd.show_status=1` 输出启动状态。
完整 KMS framebuffer 保持面板几何，桌面通过标准 DRM 接管。
窗口帧缓冲拒绝用户态打开，避免通过全屏 stride 访问窗口
以外的 UI。Polaris 不直接使用只支持 coherent/WB RAM 的通用驱动。
`DRM_MSM=m` 让 initramfs 能先建立 USB 串口，再显式加载原生显示驱动。
`reimuks.drm=0` 仅禁止 initramfs 主动加载，rootfs 仍可加载 MSM；
`reimuks.break=initramfs` 在挂载 rootfs 前停留于 USB shell。
连接器成功注册和首帧显示分别验收。

共享协议、terminal 与 fbcon viewport 主机检查已通过；真实内核的 QEMU 验证覆盖
0/90/180/270 度、滚动、窗口外和 stride 间隙保护、用户态拒绝、同步 aperture
注销及非法映射拒绝。另五项原生接管测试覆盖不同 native pitch、整页背景继承、
区域外保护、用户态拒绝及第二次停止绘制。结果绑定实际 Image 哈希；
QEMU 结果不代表实机显示已通过，设备记录单独保存。


2026-11-15 实机已通过 USB ACM、UFS 和 Debian server switch_root 验证。
关闭 ACPI 后需显式启用 CPU_IDLE、ARM_PSCI_CPUIDLE 和
ARM_PSCI_CPUIDLE_DOMAIN，否则 PSCI cluster 电源域不会注册，RPMh 下游 USB/UFS
会一直等待 supplier。配置已对照 postmarketOS 7.1_rc1-r4 核对。

原生 DRM 的黑屏已通过实机 kretprobe 定位：面板 prepare 在 DSI host 上电前
发送 DCS 命令，首条传输返回 -EINVAL。板级补丁使用
`drm_panel.prepare_prev_first` 请求标准 host-first 顺序；补丁内容计入构建身份。
构建器先断开受影响文件的硬链接，再应用补丁，固定上游 checkout 保持无改动。
连接器 registered/connected 只代表驱动注册，屏幕输出需另行实机确认。

SDM845 CPU 调频依赖 LMh 提供中断。板级配置必须启用 THERMAL、CPU_THERMAL、
QCOM_TSENS 和 QCOM_LMH；关闭 thermal 会使 cpufreq 反复延期初始化，出现 OPP
节点重复并无法注册调频策略。构建入口检查这些配置为内建。server 在 getty 开始
登录前才通过 polaris_console_fullscreen 显式扩大控制台，启动阶段继续保留交接页。

2026-11-15 LMH 配置版已在实机验证 policy0/policy4 使用 qcom-cpufreq-hw 和 schedutil，
CPU OPP debugfs 重复与 CPUFreq 注册失败不再出现；systemd 0 个失败服务、看门狗 20 秒。
登录接管已验证为旋转后的 1080×2160 全屏，启动阶段仍使用继承窗口。

瞬时黑屏的后续补丁仅尝试保留经过 KernelBoot 授权的 Polaris 固件显示。首次面板
prepare 获取电源引用后读取 DCS power mode，确认显示开启、正常模式和退出休眠后
跳过冷启动复位；校验失败、禁用 panel_novatek_nt35596s.polaris_handoff 或后续
suspend/resume 仍使用原冷启动流程。没有缩短面板必需的等待时间。
登录扩大窗口时先暂停 present worker，在 RAM 完成清理和 fbcon 重绘后一次提交，
避免显示清空后的中间帧。面板保留分支已在实机读到 power mode 0x9c 并跳过复位，
但用户仍观察到短暂黑屏；接管前的 DSI PLL 锁定失败与 PHY/host 冷初始化另行处理。


Polaris DSI 交接补丁在发布 PHY 时钟提供者前持有 runtime PM、稳压器和真实 VCO
时钟引用，避免 DISPCC orphan reparent 在无供电或无引用状态下重新编程 PLL。
仅接受经过 KernelBoot 验证的 Polaris 显示、运行且锁定的 10nm PHY0、单路
4-lane RGB888 video 模式；host 核对活动区、总时序、同步宽度与实际链路频率。
匹配时保留 PHY/PLL 和 video host，只接入 Linux 的命令 DMA 与中断。Linux
取得正常资源引用后释放临时引用；不匹配走原复位初始化，后续关闭/恢复使用
正常驱动流程。启动参数 `msm.polaris_dsi_handoff=0` 可禁用首次链路继承。
编译、模块链接、checkpatch、KernelBoot 检查与十项 QEMU 显示回归已通过。
QEMU 不模拟 SDM845 DSI 硬件；实机的 PLL、host 采用路径与可见连续性需单独验收。

DSI #11 实机在 PHY probe 读到 firmware PLL live=0、VCO=0，采用冷初始化回退；
面板仍保留 0x9c，但用户确认企鹅消失到 DRM 绿色 OK 之间有黑屏。#12 进一步
在 MDSS 创建任何子设备之前持有父域 runtime PM，覆盖 host 版本探测的临时
引用归零；Linux PHY/host 取得正常引用后才释放。SDM845 MDSS DT 没有 resets，
未盲目跳过全局 reset 或修改 GDSC 提供者。此父域解释仍需实机日志验证。
#12 编译、checkpatch、匹配运行时/组合 server、十项 QEMU 回归通过；
Image SHA256 为 1c109b864f4e85edb8825b6e1570cd9b570cdcf73febe2785a4768d448ecdf53。
记录在 build/device/20261115-dsi-handoff/r2/，硬件结果单独记录。

DSI #12 实机已证实父域保留有效：firmware PLL live=1，VCO=1839564404 Hz。
host match=0 后回退，用户看到全黑；server 仍正常运行、getty active、0 个失败服务，
这版没有通过显示验收。实际 byte/intf 为 114972775/57486388 Hz，半分频独立取整
相差 1 Hz。#13 允许该整数取整，并在采用现有链路时跳过 link_clk_set_rate，
不再用 clk_round_rate 的精确相等条件拒绝正在运行的时钟。保留完整时序、格式、
数据通道数和 PLL 锁定校验；扩充 host 状态日志。仅 msm.ko 改变，其余 1093 个模块
与 #12 相同。#13 编译、checkpatch、组合 server 与十项 QEMU 回归均通过。
硬件实测待记录于 build/device/20261115-dsi-handoff/r3/。

DSI #14 仍在采用 PLL/PHY/host 后读取面板失败（-61），用户确认长黑屏。
实验性链路继承现已默认关闭；现有 #14 二进制通过启动参数和 rootfs 的
`options msm polaris_dsi_handoff=0` 选择标准初始化。未来编译的模块默认值也为 false。
面板已有验证保留、KernelBoot 日志区域、登录全屏和电源键覆盖层不受此回退影响。
回退记录在 `build/device/20261115-dsi-handoff/rollback-standard-init/`。
