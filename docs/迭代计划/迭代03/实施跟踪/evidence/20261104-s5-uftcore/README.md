# s5 uftcore 包：Q04-002 / Q04-007 / R78-001 / NEW-030（2026-11-04）

macOS arm64，HEAD `ccaad010`（工作区含大量他人未提交改动）。本目录只记录本包定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实供应商或最终整合树。未修改任何跟踪文件。产品修改仅限 `src/WtUftCore/`（独立于 WtCore），未改 `src/TestUnits/coverage/CMakeLists.txt`。

## 构建与运行方式

- 覆盖率构建目录 `/tmp/wt-i03-s5-uftcore-cov`（默认 `-Werror` 开，Apple clang，`-DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include`）。改动后的 WtUftCore 各 TU 与全部 uftcore/m_WtUftRunner 测试 TU 在 `-Werror` 下编译通过（`logs/build-after5.log`）。
- **链接偏差**：本目录构建时他包在途改动使 `TraderCTP.cpp`（`lease` 重定义）与 `libWtDtPorter.dylib`（`DataManager::add_caster` 未定义）失败，`libWtPorter.dylib` 因而缺失（`logs/build-before-2.log`）。`CoverageTests_uftcore` 用例不引用二者，故用 ninja 的原链接命令去掉这两个 dylib 手工链接（`scripts/link-before.sh`、`scripts/link-after.sh`、`scripts/link-cmd.txt`）。m_WtUftRunner 目标正常由 ninja 构建。
- 每个用例独立 cwd、`perl alarm` 硬超时、`LLVM_PROFILE_FILE` 指到临时目录（`scripts/runcases.sh`）；`runs/<label>/summary.txt` 为逐例结论，失败例附 `out.log`。
- **红控方式**：`scripts/build-red.py` 把 `before/` 中改前 WtUftCore 六个文件放入实体 overlay（`rsync`/复制，无符号链接），用同一构建目录的原编译/链接命令把 uftcore 与 m_WtUftRunner 两个目标的全部产品与测试 TU 重编（测试为当前版本），得到 `CoverageTests_uftcore.red`、`CoverageTests_m_WtUftRunner.red`。新测试中只依赖新增诊断接口的少数断言用产品头中的特性宏（`WT_UFT_UNDONE_LEDGER`、`WT_UFT_PARSER_LIFECYCLE_CARD9`）包裹，红控构建中被编译掉，其余断言照常执行。
- 二进制哈希见 `hashes/binaries-sha256.txt`，源码前后哈希见 `hashes/before-sha256.txt`、`hashes/after-sha256.txt`，逐文件 diff 见 `patches/`。

## 修改文件（sha256 前 12 位）

| 文件 | 改前 | 改后 | 条目 |
|---|---|---|---|
| src/WtUftCore/TraderAdapter.h | 4d1deec22ed7 | e8113067ab3f | Q04-002 |
| src/WtUftCore/TraderAdapter.cpp | 16956a707e18 | 208eb84bda45 | Q04-002 |
| src/WtUftCore/UftStraContext.h | c52bf724cfbf | 48e7349be0a1 | Q04-007 |
| src/WtUftCore/UftStraContext.cpp | 87ac23787a59 | 34f83e7fd2fc | Q04-007 |
| src/WtUftCore/ParserAdapter.h | 9dd619ee1bbb | 806bc6c7a5ec | R78-001 |
| src/WtUftCore/ParserAdapter.cpp | 3e9210f07e1d | f4d0c812bec0 | R78-001 |
| coverage/q04_uftcore3/q04_trader_entrust_paths_test.cpp | 58d55d09085a | ce4e455d4e87 | Q04-002 新增 6 例；2 处旧断言/夹具按卡 5 修正 |
| coverage/q04_uftcore3/q04_trader_push_report_test.cpp | dc74851b5fd4 | c411e5ad052e | 旧断言按卡 5 修正 |
| coverage/q04_uftcore3/r11_trader_reply_deep_test.cpp | f3005800c592 | db0a863bdcff | 夹具身份与旧断言按卡 5 修正 |
| coverage/q04_uftcore3/r54_trader_callback_gaps_test.cpp | d787bf0d2aba | 29ef0fa4b3c0 | 旧断言按卡 5 修正 |
| coverage/q04_uftcore3/q04_uftctx_local_report_test.cpp | d521090c0e3d | a7f42b6c910e | Q04-007 新增 4 例 |
| coverage/q04_uftcore3/r78_uftcore_adapter_init_test.cpp | 009654d1ae81 | 9b74bfb53709 | R78-001 新增 2 例；1 例旧断言按卡 9 修正 |
| coverage/q04_uftcore3/r11_parser_adapter_test.cpp | d41fa2909fc4 | 6e395e2bec01 | 按卡 9 改用确认型 remover |
| coverage/q04_uftcore3/r54_uftcore_guards_test.cpp | 1b786fc936b9 | cf44106ebace | 同上 |
| coverage/n03_uftcore_index/n03_uftcore_helper_adapter_test.cpp | b585c29ebd51 | 3165154c4888 | 同上 |
| fixtures/i03_uft_parser_lifecycle_20261104s5/（新增） | — | 见 hashes | R78-001 动态模块 V2/TSan 夹具 |

`q04_trader_fixture.h`、`q04_trader_risk_limits_test.cpp` 只做了改前备份，内容未改（前后哈希相同）。所有被改写的旧断言在 `before/` 保留原文，改写处注释写明卡号与旧期望。

## BUG-Q04-002：UFT 逐单未完成量账本（卡 5）

实现（`TraderAdapter`）：每笔本地委托一个账本条目（node-based `unordered_map`，引用在其他条目增删时稳定），由 `doEntrust` 在调用 `orderInsert` 之前预占数量（四个下单入口不再预加聚合量，风控拒单无需回滚）；`orderInsert` 期间以 in-flight 表记录同步回报：只见拒单/撤单且无接受证据→终结并返回 `UINT_MAX`；见到存活委托或成交→即使返回负值也保留；无回报且返回负值→终结、恰好释放一次。订单回报与成交回报是两条独立的累计通道，剩余量 = 委托量 − max(订单回报累计成交, 去重成交和)，只减不增；首个终态释放该单剩余并成为墓碑，重复/迟到/存活回报不再改变。成交按 (订单, 成交号) 去重，重复成交连同持仓、统计与 sink 通知一起只处理一次；无成交号无法去重则按到达应用。usertag 严格解析为“本适配器前缀 + `.` + 十进制 id”，`uft1` 不再认领 `uft10` 的回报。无法归属（未知 id/外部 tag）的拒单、撤单、成交不改变任何订单的未完成量。`onRspOrders` 不再清空聚合量：快照行只能使剩余量单调下降或终结；只有创建与最后更新都早于查询纪元（`queryOrders` 发出时的版本）的存活条目在完整快照中缺席才标 Absent（释放其数量，更新的实时回报可复活）；同纪元分页互不判缺席；`NULL` 查询不改变任何状态。墓碑上限 65536 FIFO，回收后的 id 只会向“高估未完成量”的保守方向偏离（静态分析，未构造 65536 单的运行用例）。

红绿：
- 改前二进制 `runs/before-trader`：118 例 7 败，其中 5 例为本项反例（同步 -1 不回滚、重复拒单误扣另一活单、重复成交、重复撤单、旧快照撤销成交）。
- 红控（改前产品 + 当前测试）`runs/red-trader`：124 例 15 败——上述 5 例、新增 6 例（成交双通道计一次、撤单后迟到存活不复活、无法归属成交不改量、重复成交只记一次持仓/通知、插入期间先报存活后返回负值仍保留、快照缺席按查询纪元）、按卡 5 改写的 3 例旧断言、以及与本项无关的既有失败 `R11TraderDeep.logErrorAndLogoutAreNarrow`。
- 改后 `runs/final-trader`：124 例 123 过，唯一失败为上述既有无关用例（期望 `onLogout` 不广播断线，产品自迭代 02 起会广播，非本包条目）。

改写的旧断言（均与卡 5 逐单账本冲突，`before/` 保留原文）：`usertag_pattern_is_scoped_per_channel`（快照不再清空，A 自己的 1 手保留）、`cancel_all_filters_alive_orders_by_full_code` 与 `R11TraderDeep.orderResponseFiltersBadEntries`（旧夹具让同一 usertag 先死后活，改为两笔独立委托，原期望值不变）、`trade_push_updates_position_volumes_for_both_sides`（无法归属成交不再把聚合量扣成 −4，改为 0）、`R11TraderDeep.pushOrderCancelWrongAndAutoCancel`（撤单终态释放剩余，旧期望留下尚无成交回报的 1 手，改为 0）、`R54TraderCallback.entrust_errors_cover_action_text`（空 tag 拒单无法归属，未完成量保持 1；该例改前即失败）。

仍未闭合：UFT 侧没有卡 5/6 的账户门禁、WAL 身份持久化与跨重启恢复；“资源上限受控停账户”无载体；真实 provider 回报时序、跨线程压力（TSan 未覆盖 TraderAdapter）、Windows 未验。决策点见 `决策卡草稿.md` 卡 Q04-002-A。

## BUG-Q04-007：切日事务化重载（`UftStraContext`）

实现：`load_local_data(tradingday)` 分三段——准备（映射并校验四个块的头部 `flag/size/capacity/文件长度`、解析手工文件、在副本上计算切日 rollover、构建新的持仓汇总；任何失败直接返回，不写已有记录、不发布）、提交（仅内存写：rollover/手工覆盖写回持仓块，委托/成交/回合块按日期清理）、发布（一次性替换四个映射、持仓汇总与交易日，旧映射在发布后才释放）。`_order_ids` 保留订单号但清空指向旧映射的 `OrderStruct*`。交易日只随成功发布推进，失败时不通知策略就绪、下次 `on_channel_ready` 重试。新增 `_local_mutex` 串行化回报线程、策略线程与切日对 `_positions/_order_ids/块/_tradingday` 的访问，策略回调均在锁外执行。热切日后的状态与冷启动在同一文件上推导的状态一致（切日平仓盈亏归零沿用持久化 rollover）。手工文件在事务内解析，发布成功后覆盖并归档；归档失败记错误（不再抛出异常）。映射异常（如路径为目录）不再从 `on_channel_ready` 抛出。

红绿：改前 `runs/before-ctx` 35 例 2 败（原跨日 4→8、`R143UftCtxFallback` 目录映射抛异常）；红控 `runs/red-ctx` 39 例 6 败（上述 2 例 + 新增 4 例：切日后平仓各明细只用一次、热切日等于冷启动、映射失败不推进且可重试、切日遇手工文件为覆盖而非叠加）；改后 `runs/final-ctx` 39/39。

仍未闭合：手工文件解析失败/未知合约/归档失败与活单迟到回报语义（决策卡 Q04-007-A a–d）；本地块容量越界（见“新发现”）；多进程/崩溃恢复、TSan 未覆盖 `UftStraContext`、真实柜台未验。

## BUG-R78-001：活动 parser 回调停止屏障与析构责任（卡 9）

实现（`ParserAdapter`）：
- 宿主回调准入：所有 `handle*` 回调进入时登记 in-flight 并检查准入（seq_cst），`release()` 先关闭准入；在本适配器回调内调用或仍有回调在途时只请求停止并标记 pending、立即返回（非阻塞、可重试），回调全部退出后的下一次 `release()` 才拆除，`_stub/_bd_mgr/_cfg` 不会在回调运行中被清空。
- 动态模块必须提供合法的 parser lifecycle V2 表（校验 major/size/能力位/保留字段/回调指针并 preflight），否则在 `createParser` 前拒绝；销毁只经 `request_stop→can_wait→wait_drained(0)=DRAINED→destroy_api`，PENDING/WOULD_DEADLOCK 可重试，其余粘滞隔离；只有经证明销毁后才卸载模块。
- `initExt` 无 remover：无排空证明，`release()` 后 API 隔离至进程退出（`hasApi()` 恒真，`isQuarantined()` 真）；带 remover：remover 置空视为 provider 的销毁确认（决策卡 R78-001-A c 待确认）。
- `hasApi/isInitialized/isTeardown*` 改为非内联：改前 `CoverageTests_uftcore` 链接的 `libWtExecMon.dylib` 导出 WtCore 版 `wtp::ParserAdapter::hasApi() const`（强符号），测试可执行文件中 UFT 版内联弱定义在运行时被 dyld 绑定到 WtCore 实现（`lldb` 确认 `bl` 走 symbol stub），导致基线 24 例 parser 用例失败（`runs/before-parser`，13/37 过）。

红绿：
- 缺符号动态库卸载：`R78UftParserAdapterInit.missing_parser_factory_releases_config_and_module_reference` 改后通过（标记 `LU`，本机实际卸载）；红控因上述插桩冲突失败。
- 红控 `runs/red-parser` 39 例 26 败；改后 `runs/final-parser` 39/39（含新增 `release_waits_for_active_callback_before_teardown`、`release_inside_callback_is_deferred_to_safe_point`，以及按卡 9 改写的 `release_does_not_register_a_null_spi`：无 remover 不再 `release()`/析构）。
- 独立夹具 `src/TestUnits/fixtures/i03_uft_parser_lifecycle_20261104s5/`（`run.sh <cov-build> <dir> <before> [thread]`，provider 复制自 WtCore V2 夹具）：当前版 7/7（普通与 Homebrew LLVM 22 TSan 均 exit 0、无 TSan 报告）；改前版 7 例中普通构建 6 例红，TSan 构建 7 例全红（并发用例报告 `ParserAdapter::release()` 与回调线程数据竞争）。见 `runs/fixture-plain*`、`runs/fixture-tsan*`。
- 旧断言改写：r11/n03/r54 中依赖“`initExt` 裸 API 在 `release()` 时被销毁”的用例改为传入确认型 remover（与卡 9 一致；断言值不变）。

**m_WtUftRunner 回归（非本包文件，未修改）**：`runs/final-runner` 58 例 15 败，其中 11 例与红控 `runs/red-runner` 相同（r95/r85 配置用例，改前即失败，与本包无关）；4 例为本改动引起：`WtUftRunnerParserOwnership.duplicateRegistrationKeepsInitializedCandidateQuarantined`、`WtUftRunnerParserStart.{shutdownWaitsForParserStartAndDoesNotHoldGateLockInPlugin, sameThreadShutdownReentryDoesNotWaitOnItself, parserCallbackShutdownDoesNotWaitForConnectThread}`——它们用裸 `initExt` 假 API 并期望 runner 关闭时销毁（与卡 9 冲突）。建议补丁 `patches/SUGGESTED_not_applied_m_WtUftRunner_r220.diff`（传入确认型 remover），已在旁路二进制中验证相关 3 个套件 6/6（`runs/suggested-runner`），未应用到工作区。

仍未闭合：真实 provider 排空、Windows/Linux、WtUftRunner 析构在 PENDING/隔离时 `std::terminate`（见“新发现”）、`R130LifecycleParser` 夹具无 V2（决策卡 R78-001-A d）。

## BUG-I02-NEW-030（Z 类，卡 15）

z15 复查（`../20261104-z15-final-review/runs/new030_llvmasan_suites`）中 27 例全插桩 ASan 无任何 ASan 报告，5 例失败全部是 `q04_trader_entrust` 的未完成量记账断言（同步 -1、重复拒单、重复成交、重复撤单、旧快照）——即本包 Q04-002 的 5 个反例。

本包按 z15 同一配方（Homebrew LLVM 22、`-fsanitize=address -fno-omit-frame-pointer`、`-include stdlib.h`、`WT_WARNINGS_AS_ERRORS=OFF`，均为本地诊断变通）在实体快照 `/tmp/wt-i03-s5-uftcore-asan/snap`（`rsync --no-links`，符号链接 0；快照中本包文件哈希与当前工作区一致，见 `hashes/asan-snapshot-sha256.txt`）全插桩重编 `CoverageTests_uftcore`：248 条编译命令全部带 `-fsanitize=address`（`logs/asan/compile-count.txt`）；他包 `TraderXTP.cpp` 编译失败致 `libWtPorter.dylib` 缺失，链接时去掉该 dylib（`logs/asan/link-asan.sh`），Homebrew 预编依赖与系统 libc++ 未插桩。结果：z15 的 27 例加本包新增 6 例共 33/33（`runs/asan-new030`），trader 相关 124 例 123 过（唯一失败为无关既有 `logErrorAndLogoutAreNarrow`），ctx 39/39，parser 39/39；全部日志无 AddressSanitizer 报告。z15 记录的 5 例记账断言失败已由 Q04-002 修复覆盖，迭代 02 的 container-overflow 在全插桩下未复现（混合插桩原始红控未重造）。卡 15 处置建议见报告。

## 新发现（编号由主线程分配）

- A：`CoverageTests_uftcore`（及任何同时链接 `libWtExecMon.dylib` 的二进制）中，WtCore 导出的 `wtp::ParserAdapter` 强符号可在运行时替换 UFT 同名类的内联成员（重复符号陷阱经 dylib 生效），改前 24 例 UFT parser 用例因此失败。本包已把 UFT 侧受影响成员改为非内联；同类风险仍存在于其他同名类（如两个 `TraderAdapter` 的内联成员），需在 coverage CMake 层面隔离。
- B：`UftStraContext::on_trade/on_order` 向四个本地块追加记录时不检查 `_capacity`（`_size++` 后直接写），单日超过 8000 条会越界写映射区（静态分析，未构造用例）。
- C：WtCore `TraderAdapter` 逐单账本（非本包文件）：①订单回报 `min(accounted, volLeft)` 与成交回报 `accounted - vol` 叠加，若同一笔成交先由订单回报（volLeft 已减）再由成交回报到达会重复扣减（静态分析）；②`wt_hashmap`（ankerl，向量存储）在 `ledgerPruneLocked` 中 erase 可能移动其他条目，`ledgerTombstoneLocked` 后继续使用的引用/迭代器在墓碑超过 65536 时可能失效（静态分析）。
- D：`WtUftRunner::~WtUftRunner()` 在 `shutdown()` 失败时 `std::terminate()`；按卡 9，parser 处于 V2 PENDING 或隔离时应保留整图到进程退出而非终止，需 runner 所有者设计“保留到退出”路径（静态分析）。
- E：`UftStraContext` 在 `stra_enter_long` 等返回后才登记订单号，`orderInsert` 期间同步到达的委托/成交回报被 `is_my_order` 丢弃（既有行为，静态分析）。
- F：`R11TraderDeep.logErrorAndLogoutAreNarrow` 旧断言期望 `onLogout` 不广播断线，与现产品不符（改前即失败，非本包条目）。

## 第二轮追加（主线程追加任务，2026-11-04 晚）

改前快照 `before2/`（本轮起点，含第一轮成果），哈希 `hashes/before2-sha256.txt`，改后 `hashes/after2-sha256.txt`，逐文件 diff `patches/round2_*.diff`。红控二进制由 `scripts/build-red.py`（`RED_DIR=/tmp/wt-i03-s5-uftcore-red2`）以 `before2/` 的 WtUftCore 文件做实体 overlay 重编得到；新接口断言以特性宏（`WT_UFT_LOCAL_LEDGER_BACKLOG`、`WT_UFT_PRE_REGISTERED_ORDER_IDS`）包裹。本轮运行结果在 `runs2/`。依赖可执行文件位置的 `R173/R183` 用例从构建树 `bin/TestUnits/` 下运行。

| 文件 | 改前 | 改后 |
|---|---|---|
| src/WtUftCore/TraderAdapter.h | e8113067ab3f | c716f196a74d |
| src/WtUftCore/TraderAdapter.cpp | 208eb84bda45 | fe42f98d38cb |
| src/WtUftCore/UftStraContext.h | 48e7349be0a1 | 235cbc79433b |
| src/WtUftCore/UftStraContext.cpp | 34f83e7fd2fc | b36abd96446e |
| src/WtUftCore/UftStrategyMgr.h | 66d7fdfb0a96 | a281b3ee232e |
| src/WtUftCore/UftStrategyMgr.cpp | 34a891dc009d | 04967de16c42 |
| src/TestUnits/fixtures/r95_uft_strategy.cpp | f0571378abcd | 747de608bc45 |
| coverage/q04_uftcore3/q04_uftctx_local_report_test.cpp | a7f42b6c910e | a95e848fdd0b |
| fixtures/i03_uft_ledger_ctx_concurrency_20261104s5/（新增） | — | 见 hashes |
| fixtures/i03_uft_strategy_mgr_v2_20261104s5/（新增） | — | 见 hashes |

### 1. TSan 与 ASan+UBSan（逐单账本、切日锁）

夹具 `src/TestUnits/fixtures/i03_uft_ledger_ctx_concurrency_20261104s5/`（`run.sh <cov-build> <dir> <before-dir> <thread|address,undefined>`，Homebrew LLVM 22，直接编译全部 WtUftCore TU；改前版用 `before/` 原始文件实体 overlay）：
- `LedgerUnderConcurrentOrdersReportsAndReaders`：3 个策略线程并发下单 600 笔，1 个 SPI 线程投递存活/成交/重复成交/全成/迟到存活与拒单（含重复拒单），读线程轮询账本；结束时未完成量必须为 0、持仓恰为成交笔数。
- `ContextLocalStateAcrossDaySwitches`：策略线程下单并读本地持仓、交易线程推 on_order/on_trade、行情线程 on_tick 盯市，主线程期间两次切日；结束时本地持仓等于成交数，且冷启动结果一致。
- 结果：当前版 TSan 2/2、ASan+UBSan 2/2，均 exit 0、无报告（`runs2/cc-tsan`、`runs2/cc-asan-ubsan`）。改前版：账本用例两种构建都以记账断言失败（未完成量被重复回报扣成负值）；上下文用例 TSan 报 `_order_ids` 数据竞争、ASan 报 `UftStraContext::on_tick` heap-use-after-free（exit 134）。
- 本轮另发现并修复：第一轮 Q04-007 漏锁 `on_tick` 的盯市段（读写持仓明细），现已纳入 `_local_mutex`。
- 局限：`TraderAdapter` 的 `_positions/_stat_map/_orderids` 仍只由 SPI 回调线程写入，夹具按单 SPI 线程设计；多 SPI 线程并发回调不在现有合同内，未覆盖。容量扩展路径未在 ASan 下运行。

### 2. I03-NEW-056：本地块容量（fail-closed，不丢账务事件）

`UftStraContext` 写持仓/成交/回合/委托块前先按上界确保容量：不足时在原文件上扩展（`truncate` 增长后重新映射，`_capacity` 更新），并把 `_positions` 明细与 `_order_ids` 中指向旧映射的指针重定位后再发布，旧映射随后释放。扩展失败时事件不写、不丢：按到达顺序进入内存积压，记 ERROR，`local_ledger_blocked()`/`local_backlog_size()` 可观察，期间新下单被拒（撤单不受影响）；后续任一事件或就绪时按序补记，补完自动恢复。未加载数据块时到达的成交同样进入积压而不再解引用空指针。
- 红控 `runs2/red2-ctx`：`local_blocks_grow_beyond_one_day_of_capacity` 越过映射写入后 SIGSEGV（exit 139）；`unextendable_block_retains_events_and_refuses_new_orders` 失败（越界静默写入、未拒新单）。改后 `runs2/f2-ctx` 两例通过；冷启动可读回全部 8100 条明细。
- 待裁定：积压恢复采用自动按序补记（事件身份完整、顺序保留）；如需按卡 6 改为人工确认后恢复，见决策卡 Q04-007-A 补充项。

### 3. I03-NEW-059：同步回报归属

`TraderAdapter` 新增 `LocalOrderRegistrar`（线程局部、按适配器匹配）：`doEntrust` 在预占未完成量之后、调用 `orderInsert` 之前，把本地单号回调给提交方；回调抛异常则不发单并释放预占。`UftStraContext` 的六个下单入口都先建 registrar 再调用适配器，返回后以 `emplace` 补登记（不覆盖同步回报已建的委托记录，不再登记 `UINT_MAX`）。与第一轮 `TraderAdapter` 同步回报口径一致：单号在 provider 调用前即已存在。
- 红控：`synchronous_fill_during_insert_is_attributed_to_the_context` 失败（同步成交被当作外单丢弃，本地持仓 0）；改后通过（持仓 2，委托块只 1 条记录）。

### 4. I03-NEW-060：`R11TraderDeep.logErrorAndLogoutAreNarrow` 逐行归因

- `handleTraderLog`×2、`onTraderError(nullptr)`：无断言，行为正常。
- `onLogout()` 后 `EXPECT_EQ(0, _lostCnt)` 失败（实际 1）：产品在 e03cbe1d 实现 BUG-R11-008 时于 `onLogout` 调用 `notifyChannelLost()` 并广播 `on_channel_lost`。迭代 02 裁定（`迭代02/缺陷裁定总表.md` R11-008）只要求登出复位 `AS_NOTLOGIN`，并明确“不在本项中擅自改变 sink 的注销通知语义”“不要把 on_channel_lost 次数作为必需断言”；WtCore `onLogout` 也不广播。故归因为**产品越出已裁定范围**，旧断言与现行裁定一致，不改断言。
- 修复：`onLogout` 只复位状态与就绪标记（不再广播）；`q04_trader_reply` 的登出复位/迟到持仓不推进查询链用例保持通过。登出是否应广播断线可作为新决策点（未裁定前保持原语义）。
- 红控 `runs2/red2-trader`：该例失败；改后 `runs2/f2-trader` 124/124。

### 5. R130 夹具（I03-NEW-061）

`fixtures/r130_parser_lifecycle.cpp` 已由 dt-parsers 补 V2 表（当前哈希见 `hashes/after2-sha256.txt`）。UFT runner 目标重编后 `runs2/r2-runner` 58/58，含 `r95_uft_parser.*`（R130 成功路径）与主线程已应用的 r220 确认型 remover 补丁。

### 6. 策略插件版本化（卡 4b=A / 9a）接入 `UftStrategyMgr`

`loadFactories` 用 `wt_stra_plugin::probeModule(..., WT_STRA_PLUGIN_V2_FAMILY_UFT, ...)`：只有通过校验的 UFT 表才调用 `api->create_factory`；旧二进制与无效表拒绝（不调用旧 creator，返回 false，重扫仍拒绝），外族版本化插件跳过且不运行其 getter，非插件模块跳过；已加载模块一律常驻。工厂由共享的 `FactoryOwner` 持有，每个策略包装经 `shared_ptr` 删除器持有它，最后一个引用释放时仅当 `hasSyncTeardown(api)` 才 `destroy_factory`，否则隔离到进程退出。`fixtures/r95_uft_strategy.cpp` 补 `getUftStrategyPluginApiV2`（声明同步拆除，旧导出保留）。
- 夹具 `src/TestUnits/fixtures/i03_uft_strategy_mgr_v2_20261104s5/`（复用 `i03_strategy_plugin_abi_20261104s5/fake_plugin.cpp` 的 UFT/HFT 变体）：当前版 4/4；改前版 3/4 红（合法表被当旧接口、旧/无效表调用旧 creator 并 SIGBUS、工厂拆除不按能力）；外族用例改前也通过（V2 变体无旧导出），不计红控。`runs2/stramgr`。
- 车道回归：`runs2/f2-stramgr` 16/16、`runs2/r2-runner` 58/58。

### 本轮运行汇总

当前版：trader 124/124、ctx 42/42、parser 39/39、stramgr 16/16、m_WtUftRunner 58/58；夹具 TSan 2/2、ASan+UBSan 2/2、策略管理器 4/4。红控：ctx 3 例新增用例红、trader 1 例（NEW-060）红、并发夹具 2/2 红、策略管理器 3/4 红。未执行全量回归。

### 清理声明

本包只删除过自己创建的路径：`runcases.sh`/夹具 `run.sh` 在 `/tmp/wt-i03-s5-uftcore-*` 下按用例重建的子目录、`build-red.py` 的 overlay 目录、测试自身的临时目录。未删除任何他人目录或 `/tmp/wt-i03-*` 其他路径。
