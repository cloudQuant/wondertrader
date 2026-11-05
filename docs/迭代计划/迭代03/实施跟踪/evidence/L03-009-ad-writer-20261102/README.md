# L03-009：AD writer 会话日期前置检查（局部修复）

本项只修改 `src/WtDataStorageAD/WtDataWriterAD.cpp`，新增专属测试 `src/TestUnits/coverage/p05_datastorage2/l03_ad_writer_date_test.cpp`；未修改共享 CMake 或台账。`writeTick` 对有合约的 tick 在 retain 和同步执行/异步入队前，要求非零 action date、存在对应 session，且 `tryGetOffsetDate` 成功；失败返回 false、输出诊断。既有无合约的 no-op 返回行为保留。处理异步任务前再次检查，`updateTickCache` 在锁、缓存扩容及修改前再次使用 checked 日期，取代同交易日分支中不检查的 `getOffsetDate`。新交易日首个 tick 也必须通过日期检查；原有交易日比较、procFlag、合法夜盘与可处理的 tick 行为未有意更改。

明确兼容变化：tick 的 action date 为 0 不再隐式替换为当前墙钟日期；session 不存在也受控拒绝。应用必须确认真实 provider 不依赖零日期或动态创建 session 的旧行为。异步入队后 caller 对 tick 的并发无同步修改仍没有合法的线程安全合同；屏障测试仅在 worker 明确阻塞期间修改，并在同步释放前建立 happens-before，不能证明任意并发修改安全。session/config/数据管理器与 writer 的并发生命周期、已接收任务的失败回执、磁盘原子性和跨平台未解决。

## 本机红绿证据

- `red-sync-asan.log/xml`：旧版日期超出 9999 年仍返回 true、修改 tick、写缓存/数据库并广播，后续合法 tick 被旧状态污染；exit 1。
- `red-async-asan.log/xml`：旧版异步请求被接受且 retain、缓存和广播，exit 1。retain 数值受调度影响，接受结果和后续缓存/广播是本反例的关键。
- `red-queued.log/xml`：旧版排队后 tick 在受控屏障期间被改为超域，worker 仍写入并广播，exit 1。
- `red-invalid.log/xml`：旧版对零日期与非法公历日期未拒绝，exit 1。`red-no-session.log` 记录旧版首次 tick 在缺 session 时进入 `updateBarCache` 空指针 ASan SEGV、exit 134；它未形成 gtest XML，不将崩溃泛化为其他环境。
- `green-final2.log/xml`：上述五项新测试与五个现有合法/拒收、procFlag、同交易日、异步 drain 正控合计 **10/10 PASS**，exit 0，LLVM 22 局部 ASan，`detect_leaks=0`。专属测试位于 p05 目录，按现有 CMake 目录 glob 应进入 CoverageTests_adstorage；本次未构建或运行该仓库目标。
- 首轮 ASan+UBSan 的 `red-sync.log` 在原 `WtDataWriterAD.cpp:1221` 对 packed `TickCacheItem` 中 `WTSTickStruct` 的未对齐构造处报 UBSan 并 exit 134，先于日期断言；故随后分别使用 ASan 建立红绿，不声称 UBSan 通过。该现有对齐问题需独立处理。

## 构建范围与复现

macOS arm64。将当前 `src/` 实体复制到 `/tmp/wt-i03-l03-adwriter-20261102/src`，根部 CMake 文件副本为本目录 `CMakeLists.txt`。源码改动前 writer 无脏差异，red 取 `git show HEAD:src/WtDataStorageAD/WtDataWriterAD.cpp`；red 与 green 均使用当前 session/time 头及相同测试源，red 日志按上述时间点保存。最终产品、两个头、专属测试与二进制 SHA256 见 `hashes.txt`，四个源文件在最终快照中与工作树同哈希。编译/链接使用 LLVM 22.1.8，实体 Boost/fmt/spdlog 头和仓内 LMDB 及工具源码；Boost/系统动态库未插桩，未创建符号链接。CMake 构建日志、首轮 UBSan 和 ASan 红绿日志/XML 均留于本目录。

```sh
/opt/homebrew/bin/cmake -S /tmp/wt-i03-l03-adwriter-20261102 -B /tmp/wt-i03-l03-adwriter-20261102/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_C_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -DCMAKE_BUILD_TYPE=Debug
/opt/homebrew/bin/cmake --build /tmp/wt-i03-l03-adwriter-20261102/build --parallel 4
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 TZ=UTC /tmp/wt-i03-l03-adwriter-20261102/build/l03ad '--gtest_filter=R4WriterADTest.L03InvalidSessionDateRejectedBeforeSynchronousEffects:R4WriterADTest.L03InvalidSessionDateRejectedBeforeAsyncAdmission:R4WriterADTest.L03MissingOrInvalidActionDateIsRejected:R4WriterADTest.L03MissingSessionIsRejected:R4WriterADTest.L03QueuedTickIsRecheckedBeforeProcessing:R4WriterADTest.WriteTickGuardsNullTickNullContractAndClosedSession:R4WriterADTest.OffsetDateAheadOfTradingDateIsAbandoned:R4WriterADTest.ProcFlagOneTurnsTotalsIntoDeltas:R4WriterADTest.ProcFlagOneOnNewTradingDateRebaselinesCache:R4WriterADTest.AsyncWriterDrainsAcceptedTicksOnReleaseAndRejectsNewTicks'
```

未验证真实 provider、Windows/Linux、运行期间同时更改 session 或销毁 manager/sink、最终整合树和整体回归；局部结果不关闭 L03-009。另有真实磁盘故障/旧异步错误传达及 AD 分钟键迁移约束，与本项检查分别处理。
