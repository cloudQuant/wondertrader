# BUG-P05-009 当前源码隔离验证（2026-11-02）

## 范围与结果

本次未修改产品源码。仓内可见引用复查：生产路径只有 `WtDataReaderAD` 私有 string 包装和 bool+输出参数重载本身，没有调用该 helper 的生产调用点；测试通过 friend access 单独调用它。旧 string 包装为兼容保留，空结果与失败均返回空串，无法承担需要区分失败的调用合同。bool 重载正常空集时返回 true 并清空输出，失败时返回 false 且保留原输出；任何未来生产调用必须显式处理 bool 结果。这不能证明生产读取链路，也不能证明仓外二进制或插件没有间接使用。

在 macOS arm64 的临时实体源码快照单独构建 reader、LMDB、工具依赖和测试，Homebrew LLVM 22.1.8 的 ASan/UBSan 插桩，`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`：5 个测试在独立进程各 1/1 PASS、exit 0，无 skip。覆盖空/单/多条数据、闭区间末端和字节逐项一致、短长 value 与异常 key、遍历中断失败不发布部分结果、无效身份和周期、新增数据库目录打开失败与有效空集的区别以及旧包装的歧义。每个日志和 XML 以 `physical-` 开头，命令与状态在 `physical-results.json`；`run.py` 使用用户的 conda base Python 复现。

新用例的受控反例只在临时副本中将“数据库不可打开”从 false 改为 true；该用例如预期 FAIL（exit 1），见 `controlled-open-failure-red.*`。此为针对新断言的受控行为反例，不是历史产品源码；生成候选时的临时源文件还将原 CRLF 归一为 LF，diff 为忽略行尾差异的语义差异。恢复原源码并重编后，以上五项重新通过。此前历史版本范围错误的红控及源码见相邻 `../P05-009/`。

## 构建与边界

- `/tmp/wt-i03-p05-009-uniform/src` 由工作区 `src/` 实体复制；测试源码追加后同步至快照。`physical-sources.sha256` 为相关源、头、测试和最终二进制的哈希；列出文件的当前工作区和构建快照一致，不意味着全工作树构建期间完全冻结。
- `CMakeLists.txt`、`build-commands.txt`、`configure-retry.log`、`build.log`、`rebuild.log`、`red-build.log` 和 `restore-green-build.log` 记录构建。首次 configure 因没有传入 Ninja 路径失败，见 `configure.log`，随后指定 `/opt/homebrew/bin/ninja` 成功。后续改用实体 `/usr/local/lib/libiconv.2.dylib` 并重链，见 `rebuild-physical.log`；该后端与 Boost 两个预编动态库和系统 libc++ 均未插桩。Boost 1.88.0 头和库编译链接路径使用 Cellar 实体；它们的 dylib install name 指向已有 `/opt/homebrew/opt/boost` 链接，运行时解析到同一 Cellar 版本。编译器及 ASan runtime 自身也使用系统已有链接，本次没有创建任何符号链接，隔离构建目录 `find -type l` 结果为空。
- 仅 macOS 本机隔离 LMDB 和所列 TU/依赖，Windows、Linux、真实 I/O 故障、并发调用、生产读取链路、最终整合树和整体回归未验证。没有把受控 cursor 故障外推为所有 LMDB 故障，也没有改变 ABI 或认定原 213 项通过最终验收。
