# AD writer packed tick 缓存对齐（局部快照）

本快照的 writer 通过对齐的临时 date 和 WTSTickStruct、按既有 packed(1) 记录偏移分别 memcpy，完成 tick cache 新增、更新、索引恢复和 getCurTick；不修改磁盘头、记录大小、字段布局或数据。静态断言固定记录布局；已有记录的 identity 未以 NUL 结束时在发布任何索引前拒绝，关闭映射，并支持修复后同实例重试。索引访问增加 size/capacity 边界检查。后续 bar cache 变更不属于本快照。源文件和哈希已保留于 `tick-fixed-source.cpp`/`hashes.txt`，与本目录 `green-final2.log/xml` 对应。

旧版分别在首条 tick 局部 packed 对象构造、第二条映射项 getCurTick 引用时报 UBSan 非对齐并退出 134（`red-first.log`、`red-recovery.log`）；旧版接收未终止 identity 的红控退出 1（`red-identity.log/xml`）；截短头/记录的原行为正控退出 0（`red-truncation.log`）。当前快照的 ASan/UBSan 相关 tick/cache/日期控制 **15/15 PASS**（`green-final2.log/xml`）；覆盖首写、第二条读取及更新、磁盘原始数据字节、合法旧缓存恢复、截断/size>capacity 不改文件、坏 identity 修正后同实例重试、procFlag 0/1/2 和异步等。ASan LSan 关闭，其他预编动态依赖未插桩。没有运行真实 provider、全仓或跨平台。

`bar-alignment-probe.log` 保留一个独立失败：在同一 tick 修复快照启用 bar 时，UBSan 于 `updateBarCache` 的映射 WTSBarStruct 未对齐访问终止（134）。该失败不被 tick 15/15 掩盖。该快照还未解决跨进程同时读写、session/config 生命周期、全量内存安全或 bar reader 的映射指针所有权。

隔离目录 `/tmp/wt-i03-ad-tick-alignment-20261102` 为实体源码复制，CMakeLists 附本目录；采用 Homebrew LLVM 22.1.8、实体 Boost/fmt/spdlog 头、当前 session/time 头，具体构建日志已归档，无新建符号链接。运行命令与 15 项过滤器见 `green-final2.log` 的开头；设置 `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TZ=UTC`。源树的 p05 目录已有 CMake glob；本快照仅用独立目标编译，未构建仓库完整目标。
