# AD writer packed bar 缓存对齐（局部快照）

在保留既有 tick 日期与 tick 对齐变更的基础上，对 m1、m5、day 三种 bar 的映射缓存恢复、首次写入、更新和输出，使用对齐的临时对象和逐字段字节复制；静态断言约束头大小、记录字段偏移与总尺寸，不改变原有磁盘格式。恢复时先验证身份字段 NUL 结尾再建立索引；畸形文件拒绝且释放该映射，修复后同实例可重试。索引使用前检查 size/capacity。

`bar-red-first.log` 和 `bar-red-recovery.log` 是固定 tick 后、修改 bar 前的 UBSan 红控，均因非对齐 bar 访问退出 134；`bar-red-identity.log/xml` 的旧实现未拒绝坏身份，测试退出 1；截断正控见 `bar-red-truncation.log`。`bar-green-final3.log/xml`：ASan/UBSan 21/21，包含 bar 首写和更新、三周期两记录旧数据恢复、刷出 fake dumper 和实际 LMDB 并在 writer 销毁后回读、截断、损坏身份修复重试、size 超容量，以及 tick、日期与已有行为相关控制。未启用 LSan，预编动态依赖未插桩。`bar-fixed-source.cpp`、`hashes.txt` 为本次快照；此前 tick-only 证据独立保存在 I03-AD-tick-alignment-20261102。

复跑 `sh docs/迭代计划/迭代03/实施跟踪/evidence/I03-AD-bar-alignment-20261102/rerun.sh`，依赖本机现存实体隔离构建 `/tmp/wt-i03-ad-tick-alignment-20261102`、Homebrew LLVM 及依赖；CMakeLists 归档于本目录，该命令不自动同步工作树。检查源码与隔离快照哈希后运行。没有运行真实行情交易、全仓目标、跨平台或第三方外部 writer 插件。

这是 writer 局部结果：AD reader 仍存在从 packed 映射返回 bar 指针的路径，不证明 reader 无对齐问题；跨进程并发读写和混合版本、持有回调参数超过同步回调的外部实现均未验证。此证据不代表 AD writer 全部路径或迭代整体验收。默认 git whitespace 将现有 CRLF 风格识别成行尾空白；使用 `git -c core.whitespace=cr-at-eol diff --check -- src/WtDataStorageAD/WtDataWriterAD.cpp` 通过。
