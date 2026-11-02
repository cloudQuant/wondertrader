# BUG-R113-002 定向验证

当前 TraderMocker 使用实例 RNG；可选 `rng_seed` 明确指定重放序列，未指定时非固定播种。默认逐笔序列及分布与旧 C rand 实现不保证相同。旧自由函数符号保留且不再改动进程全局 C rand。源码/二进制哈希和实际编译、链接命令见 `sha256.txt`、`build.log`。

macOS arm64 定向 6/6 PASS：原拆量边界三个用例、固定种子重放与实例隔离、宿主 C rand 不变及总量边界、两实例经 UDP 撮合相同逐笔序列。逐例日志及 XML 位于 `run0` 至 `run5`。主代理重新运行独立 C rand 探针，旧对象 exit 1、新对象 exit 0，见 `recheck-exits.log` 和 `recheck_*.log`；探针源码及两种链接命令见本目录。

测试仅重编了相关产品及测试翻译单元，其余依赖复用 v8 产物，不能证明当前完整工作树。真实 Windows/Linux、完整生命周期并发、整体回归未验证；活跃时重复 init 对其他配置字段和重新播种语义尚未验证。非整数或非法 minqty/maxqty 造成的边界风险需独立处理。
