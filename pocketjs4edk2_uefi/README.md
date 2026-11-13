# Polaris 组合构建边界

目录名和调度入口参照 W202DS，实际平台、显示交接和镜像封装必须由 Polaris 实现。
`build-firmware.py` 目前只提供标准参数接口并明确拒绝未实现的构建，不生成占位镜像。
接收协议沿用 REIMUKS 的 `firmware-manifest.json`，真实产物必须通过板级校验后才能发布。
