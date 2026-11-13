# Polaris

Xiaomi MIX 2S（Polaris，Qualcomm Snapdragon 845）的 Linux 与 UEFI 板级项目。

| 分支 | 内容 |
| --- | --- |
| [`main`](https://github.com/Yeamika/Polaris-linux/tree/main) | 项目导航与参考资料，只维护本 README 和 REFERENCES |
| [`boot`](https://github.com/Yeamika/Polaris-linux/tree/boot) | SDM845 UEFI、板级组合构建及 Windows / Android / LinuxBoot EFI 入口 |
| [`7.1-rc1`](https://github.com/Yeamika/Polaris-linux/tree/7.1-rc1) | Linux 板级工程，直接引用上游 SDM845 7.1-rc1 内核 |

源码分支正在准备与移植，远端尚未发布对应实现；分支存在不代表可构建或已通过实机验收。
发行版组装由独立 REIMUKS 项目承担，硬件机制、启动封装与发行版策略分别维护。

上游来源、固定提交及适用边界统一见 [REFERENCES.md](REFERENCES.md)。
