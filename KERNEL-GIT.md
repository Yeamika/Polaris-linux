# 内核来源

直接使用 https://gitlab.com/sdm845-mainline/linux.git ，来源标签为 `sdm845-7.1-rc1-r0`，
固定提交 `85f1df2a4ec71d7a91dd95a7a49f889d1595ffa8`，Makefile 版本为 `7.1.0-rc1`。
以 gitlink 和 `provenance/kernel.json` 为准，不随上游分支自动更新。

```sh
git submodule update --init kernel
python3 scripts/check-source.py
# 仅在需要完整源码时运行；可能下载大量缺失对象：
git -C kernel sparse-checkout disable
```

目录初始化和源码身份检查不代表配置、编译或实机启动通过。内核输出需与其 modules、
.config 和身份 JSON 一起交给 REIMUKS，不能使用旧 6.12 产物补缺。
