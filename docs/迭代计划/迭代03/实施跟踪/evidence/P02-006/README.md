# BUG-P02-006 定向复验（2026-11-01）

本机 macOS arm64。当前工作区 WtDiffExecuter.cpp 和指定测试 TU 重新编译；Boost 头使用 v8 构建目录内的实体目录，检查目录及其所有后代均非符号链接。测试使用隔离进程、独立工作目录和 TMPDIR。未修改工作树。

- `P02DiffExecuterTest.LoadDataRejectsMalformedSnapshotsWithoutPartialRestore`：1/1 PASS，进程退出 0。
- `P02DiffExecuterTest.LoadDataAcceptsCurrentArrayAndHistoricalObjectMap`：1/1 PASS，进程退出 0。
- `P02DiffExecuterTest.PartialWriteFlushAndCloseFailuresPreserveLastGoodAndRetry`：1/1 PASS，进程退出 0。

完整编译和链接参数及工作目录在 commands.json，源码 SHA-256 在 hashes.json，逐例命令、退出码和 XML 摘要在 results.json，各例输出和 XML 分别在 run0、run1、run2。构建期间及运行后六项记录源码哈希未变化。产品依赖清单 prod.d 显示工作区当前 BoostFile.hpp 被编译使用。当前产品与测试对象排在链接其他对象之前以优先选择当前弱内联定义。

范围限制：其他预编对象和动态库复用 v8 构建，非最终工作树完整冷构建；短写、fflush 和 fclose 故障为可控 hook，不证明真实断电、硬件故障或跨平台持久性；未运行全量回归或其他测试。
