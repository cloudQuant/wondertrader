# exec-units 包（执行器与执行单元）定向修复证据（2026-11-04，s5）

macOS arm64，HEAD `ccaad010`（工作区含大量他包未提交改动）。本目录只记录本包定向修复与定向用例，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实供应商或最终整合树。未修改任何跟踪文件；未修改 TraderAdapter.*、AccountTradingGate.h、ExecOwnerModes.h（WP1 所有，只读使用 `TraderAdapter::accountGate()`）。

负责条目：BUG-I02-NEW-035、BUG-R7-002、BUG-R190-001、BUG-R7-012（E）、BUG-R17-001、BUG-R43-002、BUG-R202-003、BUG-P02-005、BUG-I02-NEW-053、BUG-R7-004（Z）；追加 I03-NEW-014。

## 1. 快照与构建

- 改前文件 `before/`（保留相对路径）；哈希 `hashes/before-owned-product-sha256.txt`、`hashes/before-tests-sha256.txt`；改后 `hashes/after-sha256.txt`；差异 `patches/*.patch`。
- 共享工作树构建（`/tmp/wt-i03-s5-exec-units-cov`）被他包进行中的改动阻断：17:41 `WtDtCore/DataManager.h/.cpp` 不一致（WtDtPorter 编译/链接失败，17:45 后一致）；`q01_wtcore3/r15_hft_stra_context.cpp` 访问 `HftStraBaseCtx::kRetiredOrderTagLimit`（protected）编译失败（他包进行中，见新发现 N-5）。均与本包无关。
- 改用隔离快照：`scripts/make_snapshot.sh`（`rsync --no-links` 复制 `src/`、`dist/common`，`find -type l`=0）。快照内唯一构建调整：coverage CMake `wtcore` 行收窄为执行器相关 24 个测试文件（列表见脚本），绕开 r15；R202-003 合同用例只在快照内并入 n01 lane。默认配置（`WT_WARNINGS_AS_ERRORS`=ON，`-Werror`），`-j 3`。快照 `/tmp/wt-i03-s5-exec-units-snap-green`，构建 `/tmp/wt-i03-s5-exec-units-greenbuild`。绿态快照源哈希 `hashes/snapshot-green-sha256.txt`，二进制哈希 `hashes/green-*.sha256`。
- 红控：`scripts/swap_product.sh <snap> before` 只把本包产品源换回 `before/`（测试与新增头保持当前），同一构建目录增量重编后运行同一用例集；完成后 `swap_product.sh <snap> after` 复原。
- 另以共享构建编译参数对全部新增/修改测试源做 `-fsyntax-only -Werror`，均 exit 0（新文件进入共享树后不阻断他包构建）。
- 运行：`scripts/runcase.sh` / `scripts/run_all_cases.sh`，每例独立 cwd（`/tmp/wt-i03-s5-exec-units-run/...`）、`LLVM_PROFILE_FILE` 指向该 cwd、`perl alarm` 硬超时，`-P3`。R59 的真实插件用例设 `WT_COV_PLUGIN_BIN=<build>/build_x64/Debug/bin`。

## 2. 任务1：CoverageTests_wtcore 既有失败逐例归因

主线程日志 `/tmp/wt-i03-s5-run/wp1-related/cwd/<用例>/log.txt`（二进制 `42242364…`）。

| 用例 | 现象 | 根因 | 处置 |
|---|---|---|---|
| P02LocalExecuterTest ×9、R59Exec 三个 Local 用例、R73ExecTest.LocalAccessorsAndGuards/LocalAutoClearEarlyExits | `libc++abi: terminating`（134） | 夹具按值/栈持有 `WtLocalExecuter`，未经协调 quiesce 即析构；`~WtLocalExecuter` 自迭代02（e03cbe1d）起对未 quiesce 宿主 fail-stop（卡9 9a=A+C/9b=A：确认停止后才销毁） | 测试：经 `WtExecuterMgr`+`WtExecQuiesceCoordinator`（不拥有的 shared_ptr 条目）在析构前 quiesce，随后注销 sink。产品（见 3.1）：未知工厂等宿主侧预检失败不再记为“进入插件代码”，否则这些空工厂宿主永远无法 quiesce |
| R10ExecuterTest ×16 | `init` 返回 false | 卡8b=A eager 校验（R10-001/P02-002 已落地）：`scale` 必须为有限正数（旧实现缺省读成 0）、policy `name` 严格 `<factory>.<unit>`；夹具缺 scale、名称无工厂段 | 按卡8b=A 补 `scale`、名称改 `R10Fact.R10Unit`（工厂仍未加载） |
| R10ExecuterTest.diff_load_data_broken_json… | init false | HEAD（迭代02）Diff 快照先整体 schema 校验再发布，损坏/空快照拒绝（与 R15 SEL 快照同一合同，亦合卡13c=A fail-closed）；旧 oracle 期待静默忽略并以空账本覆盖 | 改名 `…_are_rejected_without_overwrite`：断言 init 拒绝、配置引用不保留、原文件不被覆盖 |
| R59Exec.diff_stale_target_* ×2 | `cannot locate build-cov plugin bin` | 环境：用例经 `WT_COV_PLUGIN_BIN` 或 `<repo>/build-cov` 定位 libWtExeFact，主线程运行未设 | 设 `WT_COV_PLUGIN_BIN` 后通过；非产品问题 |
| R59Exec.set_trader_forwards_orders_only_while_channel_ready | （原先 abort；修 quiesce 后）Arbi/Diff 下单为 0 | 卡5b=A/5c=A（WP1 已实现）：无宿主身份能力单元时以非共享 owner 下单，同账户同合约被首个 owner 独占；旧 oracle 让三个执行器同账户同合约各下两单 | Local/Arbi 用不同合约，Diff 用独立 adapter（私有账户门禁）；另断言同合约第二 owner 被拒 |
| R71ExecTest.ConcurrentSameCodeCreationAndPreRegistrationChannelEdges | 60/120 s 超时；`waiting_callers` 0≠1 | ①旧 oracle：2026-11-03 Diff 批次重构后目标先落账再非阻塞请求单元，同码调用不再阻塞；②产品挂起：`on_channel_ready` 对已有差额的代码调用**阻塞** `getUnit`，等待在途创建者，而创建者被测试阻塞到 ready 返回之后 → 死锁 | 产品修复见 3.3（卡9d=A）；oracle 按非阻塞记账更新（枚举 `DIFF_RECORDS_TARGET_BEFORE_REGISTRATION`） |
| R71ExecTest.DiffFailedInitializationWaitersShareOneGeneration | exit 93 | 同上①：旧 oracle 期待 6 个调用方阻塞等待 | oracle 改为调用方记账后返回、同代失败、commodity 查询仍 1 次、quiesce 仍不完成（保留原断言） |
| R71ExecTest.DiffV2FailedInitializationStillRequiresSafeAutomaticRetry | exit 94（未启动新代） | 产品缺口：Diff 进入插件代码前置 `retry_blocked=true` 且永不放开 | 产品修复见 3.2 |
| R71LocalSerialTest.PreflightFailureBeforePluginEntryCanBeCleared | init false | 夹具无 policy，卡8b=A 下 init 即拒绝，预检失败场景不可达 | 改为合法 policy、工厂未加载（宿主侧预检失败），依赖 3.1 |
| R90ExecPool.Local/ArbiPoolForwardsEverything | n_insert 0 | 迭代02 起 `setTrader` 只绑定，通道就绪经已注册 sink 回调送达；旧用例只 setTrader 不 addSink，执行器通道始终未就绪。另卡8/P02-002 整表目标会把 ag 清零 | 按 runner 接线 addSink（结束 quiesce 后 removeSink），并在 ag 目标为 2 时先送一笔 ag 行情 |
| R15MgrTest.local_executer_on_tick_is_safe_with_empty_unit_state_map（复跑中发现） | init false | 同卡8b=A | 补 scale、名称改 `R15NoSuchFact.R15NoSuchUnit` |

所有旧 oracle 改写均保留 `before/` 原文件；未删除断言，改写处注明卡号。

## 3. 产品修改

### 3.1 宿主侧预检失败（WtExecuterFactory + 三执行器）
`WtExecuterFactory` 新增非虚重载 `createExeUnit/createDiffExeUnit/createArbiExeUnit(name, bool& pluginEntered)`：仅当控制权交给插件 `create_unit` 时置 true；名称非法、单元准入已关闭、工厂未加载属宿主侧拒绝。Local 据此不再置 `externalCodeEntered`（可清除、可重试、不使 quiesce 失败）；Diff 复位 `retry_blocked`；Arbi 复位 `side_effects_possible`（仅放开重试，quiesce 判定不变）。Local `failCreation` 另规定：宿主侧预检失败时，排队中的目标对账/清仓事件是权威 `_target_pos/_pending_clear_position` 的投影，新一代注册时重新排队，丢弃不算丢失已接收工作；通道事件及其他事件仍按原规则计入（R71 “queued ready callback fail-closed” 用例语义不变）。类布局与插件 ABI 不变。

### 3.2 Diff V2 安全自动重试（NEW-035 / R7-002 / R190-001）
`WtDiffExecuter::getUnit` 的 `failCreation`：仅当 ABI 2.2 证明失败初始化已同步销毁（`failedInitializationWasSafelyDestroyed()`）、本代从未注册（`_unit_map` 无映射）、且无已接收回调工作（`callback_events/callback_active/callback_failed/result` 均空）时复位 `retry_blocked`，后续请求开启新一代；旧 ABI/未证明清理的失败仍 fail-closed 并持久化墓碑。账本并发落账：目标/差额在 `submitPositionTargets` 的 `_position_update_mutex` 临界区内先整批提交再非阻塞请求单元；`registerCreatedUnit` 与本失败路径持同一互斥量读取最新差额。故跟踪备注“失败调用先 getUnit、返回后才写 `_diff_pos`”所述竞态在当前实现中不再成立（落账先于请求；waiting_callers 不再作为提交证明），持锁期间不运行插件代码（无回调重入死锁）。

### 3.3 Diff `on_channel_ready` 不阻塞在途创建
对已有差额的代码改为 `getUnit(code, true, false, &pending)`；在途创建由 `registerCreatedUnit` 在同一更新锁下读取 `_channel_ready` 并排队 ready+恢复，仅非 pending 的失败才标记 quiesce 不完整（卡9d=A：同步重入不就地等待）。

### 3.4 账户净额门禁（P02-005，卡8 补充裁定）
新增 `src/WtCore/AccountNetPositionLimits.h`：进程内按账户身份（`TraderAdapter::accountGate()` 共享门禁对象；无 trader 时执行器自身为作用域）分区，保存各执行器已提交批次的物理目标（交易数量，已乘 scale），每个实际合约分别判断 |账户净目标| ≤ 限额，不同合约不相加；限额来自执行器配置 `account_limits`（键：完整合约 > `交易所.品种` > `default`），同账户多份声明取最小；不增加 |净额| 的变化（减仓、清零）总是允许。Local/Arbi 在组限额（`execution_groups::calculate`）之后、提交临界区内原子检查并发布；Diff 在整批 `nextTargets` 提交前检查，`setTrader` 时把 `load_data` 恢复的目标不经检查发布给同账户其他执行器；三者析构时注销。`validateConfig` 对 `account_limits` 做 eager 校验（卡8b=A）。限制：这是目标层（计划敞口）门禁；执行器之外的持仓变化、其他进程、adapter 层在途订单一致性不在此覆盖（需 WP1 在 TraderAdapter 下单入口提供账户级接口，见报告）。

### 3.5 TWAP 合法交易单位（R7-012，卡E）与 TWAP/VWAP 清仓分支（R7-004）
`WtTWapExeUnit`：常规切片与撤单重发都按商品 `lotstick/minlots` 向下取最大合法数量（T+1 先受可用量约束），不足一个合法单位的余量保留未完成、记录诊断，不发送不合法数量，不假设零股；元数据非法时不发单。卡15复核发现明确缺陷：清仓指令（DBL_MAX）在净持仓为零但有锁仓时，原代码因“差额为零即返回”使清仓分支不可达；现清仓时继续进入该分支，先以 forceClose 卖出可用多头，空头经普通路径买回；`WtVWapExeUnit` 同一缺陷同样修复。

### 3.6 重复终态回报（R17-001）
`WtOrdMon` 订单记录增加 `terminal` 标志：`mark_order_terminal` 仅首次转终态返回 true，同号新订单登记时复位；`WtDiffMinImpactExeUnit::on_order` 对重复终态回报直接忽略（不再重复递减撤单计数、不重复撤单价格惩罚和重算）。

### 3.7 回测盘口簿（R43-002）
`MatchEngine::update_lob` 每笔 tick 以该 tick 的完整档位快照重建（含空盘口清空），不再保留不再显示的旧档位。该簿不参与撮合，撮合行为不变。

## 4. 修改文件哈希（sha256 前 12 位）

见 `hashes/after-sha256.txt`；表格见第 12 节。

## 5. NEW-053：设 LLVM_PROFILE_FILE 时严格 LSan exit 23 的根因（已确证）

结论：泄漏来自**覆盖率插桩动态模块自带的 compiler-rt profile 运行时**，非产品或夹具泄漏。每个以 `-fprofile-instr-generate` 构建的 dylib 静态链接一份 profile 运行时；`dlopen` 时其静态初始化 `_GLOBAL__sub_I_InstrProfilingRuntime.cpp → __llvm_profile_initialize → parseAndSetFilename` 对 `LLVM_PROFILE_FILE` 模式串 `strdup`，指针只保存在该模块数据段；`dlclose` 卸载模块后该指针随映射消失，LSan 报告“已卸载模块中的 strdup 直接泄漏”，大小恰为模式串长度 + 1。

- 最小复现（与 WonderTrader 无关，`new053/plugin.c`、`host.c`、`host_sym.c`、`commands.sh`，Homebrew LLVM 22.1.8，`ASAN_OPTIONS=detect_leaks=1`，`LSAN_OPTIONS=exitcode=23`）：A 插桩模块+设变量+dlclose → exit 23、55 字节（模式串 54）；E 长模式串 → exit 23、104 字节（103+1）；B 不设变量 → 0；C 不 dlclose → 0；D 非插桩模块 → 0。F 在 dlclose 前记录模块基址，`atos` 把三帧未知模块地址符号化为 `parseAndSetFilename+212 / __llvm_profile_initialize+56 / _GLOBAL__sub_I_InstrProfilingRuntime.cpp+8`（`new053/run/F_symbolize/`）。
- 原用例对照（只读运行 reverify 的全插桩 ASan 覆盖率二进制，独立 cwd，`new053/wt/`）：`P11TraderTest.init_rejects_overlong_ids_and_keeps_empty_id_accepted`，模式串 54 → exit 23、55 字节；109 → exit 23、110 字节；不设变量 → exit 0；三次 gtest 均 1/1 通过。被 `TraderAdapter::init` 加载的 `libR59CredentialsTrader.dylib` 含 `___llvm_profile_initialize`/`_parseAndSetFilename`（`new053/wt/plugin-profile-symbols.txt`）。
- 建议：严格 LSan 验收不设 `LLVM_PROFILE_FILE`，或用非覆盖率构建的插件；该现象不应计为产品泄漏。NEW-053 其余子项：R59Exec 的 abort 已在本包修复（第 2 节）；NEW-052 等产品泄漏仍独立跟踪。

## 6. R202-003（卡8c=A）：需 WP1 修改 TraderAdapter

合同：风控开启时未知代码（基础数据解析不到实际合约）与排除集代码拒绝下单；风控关闭保持允许。排除集在当前实现已拒绝。未知代码：`TraderAdapter::isTradeEnabled/checkOrderLimits/checkCancelLimits` 在风控开启时经 `getRiskParams` 回退到必需的 `default` 规则后放行。该文件属 WP1，本包未改。
合同用例 `src/TestUnits/fixtures/i03_exec_units_20261104s5/r202_003_risk_unknown_code_test.cpp`（只在快照并入 n01 lane，使用本地 TraderMocker）：`RiskOffAdmitsKnownAndUnknownCodes` 通过；`RiskOnRejectsUnknownCodeAndKeepsKnownCode` 当前红（`isTradeEnabled/checkOrderLimits("SHFE.xx.9999")` 均为 true）。所需修改（建议）：风控开启时，三个入口先用 `_bd_mgr`（为空则拒绝）按 `CodeHelper::extractStdCode` 解析实际合约，解析不到返回 false 并记录诊断；`buy/sell` 入口同样在解引用 `cInfo->getCommInfo()` 前判空（当前未知合约下单会空指针解引用，见新发现 N-3）。WP1 落地后把该用例移入 n01 lane 复跑红→绿。

## 7. R7-004（Z，卡15 最终复核）

复核 WtExeFact 各单元的清仓（DBL_MAX）路径：MinImpact 在净持仓等于目标时进入清仓分支，先以可用多头强平，空头经普通路径以 forceClose 买回；**TWAP 与 VWAP 的同一分支被前置“差额为零即返回”挡住而不可达**（既有 `r7_vwap.clear_target_dbl_max_exits_without_order` 注释已点名 BUG-R7-004），净零锁仓（多空毛仓并存）永不平——属明确缺陷，已在两单元修复（3.5），补正控 `r7_twap_clear.net_zero_locked_position…`、`r7_vwap.clear_target_flattens_net_zero_locked_position`（红→绿）及平仓/普通零差额对照（红绿均通过）。StockVWAP 同样早返回，但股票无空头、净零即无持仓，无锁仓场景。仍无合同依据：可用量不足（冻结/T+1）时锁仓空头的处理、仅单向账户时是否允许方向平仓（ExecuteContext 无账户模式/冻结量查询）、股票卖空规则。建议：缺陷修复计入本条证据；其余子项按卡15记“用户指示忽略”，状态保持阻塞，解除条件为取得账户双向/冻结量接口合同。

## 8. I03-NEW-014（Diff `_config` 构造初始化）复验

构造函数已有 `_config(NULL)`（11-03 修复，本包快照前已在）。新增 `q01_wtcore3/i03_new014_diff_config_init_test.cpp`：8 轮在堆上构造 Diff、init(NULL)/非法配置失败后收到目标，`getUnit` 必须在进入工厂前以 “executor configuration is missing” 失败。绿构建在 `MallocPreScribble=1 MallocScribble=1 MallocGuardEdges=1`（新分配填 0xAA，未初始化成员会被解引用）下 1/1 通过（`runs/green-new014/`），全量收窄运行中亦通过。**ASan 未在本包构建中重跑**：机器负载 100–200，整树 ASan 构建不可行；原 ASan 红控（`_config=0xbebebebe…`）与修复后 ASan/UBSan 5/5、15/15 见 `src/TestUnits/fixtures/i03_exec_eager_validation_20261103/README.md`。建议状态：本地验证通过（以 malloc 填充替代 ASan 的局限如实记录），或待验证直至补 ASan。

## 9. 新发现问题（请主线程分配编号；未扩大范围修复）

- N-1 Diff quiesce 永久不完整：`WtDiffExecuter::getUnit` 的 `failCreation` 无条件置 `_quiesce_incomplete`，`submitPositionTargets` 在创建者失败时再置一次；因此即便失败可安全重试且随后新一代成功，执行器也再不能完成协调 quiesce（runner 只能保留 owner 到进程退出）。见决策卡草稿 D-2。
- N-2 Diff 在 init 失败后仍接受目标并落盘：`set_position/on_position_changed` 无初始化门禁，`save_data_locked` 会用新账本覆盖刚被 schema 校验拒绝的快照（原文件本应留待人工核对）。复现：写入损坏快照 → init 返回 false → 非空 `set_position` → 文件被改写。建议 `_config == NULL` 时拒绝目标。
- N-3（WP1 文件）`TraderAdapter::buy/sell` 在 `getContract` 返回空时直接解引用 `cInfo->getCommInfo()`，未知合约下单空指针崩溃；与 R202-003 一并处理。
- N-4 Diff 的 `on_tick/on_trade/on_order/on_entrust` 以 `getUnit(code,false)`（默认等待创建）在 adapter 回调线程上阻塞等待在途插件初始化；与本包已修的 `on_channel_ready` 死锁同类（卡9d=A），仅静态分析，未改。
- N-5 共享树构建：`q01_wtcore3/r15_hft_stra_context.cpp:1146` 访问 `HftStraBaseCtx::kRetiredOrderTagLimit`（protected）编译失败（他包进行中改动，17:4x 观察）。
- N-6 exefact 目标 16 例、btcore 收窄目标 18 例失败与本包改动无关（红绿完全一致，见第 11 节）：r7_twap ×8/r24_twap 的旧 fake 以单一 `pos` 表示多空，触发 TWAP 持仓一致性校验或撤单重发受剩余量约束；r220 ×3（含 SIGSEGV）、r115、r24_runner、r7_vwap_profile、r7_vwap.unrepresentable 及 R111/R121 ExecMocker（回放器 init 失败/夹具插件）均需另行归因。

## 10. 决策卡草稿

- D-1 账户净额门禁的配置位置与口径（P02-005）。要决定：上限配置放执行器（已实现 `account_limits`，同账户取最严）还是账户/adapter 级（需 WP1 在 TraderAdapter 读取并提供只读接口）；是否在目标层之外再于 adapter 下单入口按“实际持仓+在途订单”做账户级拦截。选项：A 仅执行器目标层（现状）；B 账户级配置+目标层；C B 再加下单入口持仓/在途检查。建议 C（目标层已就绪，下单层由 WP1 提供 `accountNetCheck(code, delta)` 类接口后接入）。影响：P02-005、R202-003、卡5c。
- D-2 Diff 安全失败后的 quiesce 语义（NEW-035/R7-002/R190-001 关联）。选项：A 保持任一失败即永久不完整（现状，fail-closed，资源保留到进程退出）；B 仅对“可证明无副作用”的失败（宿主预检、V2 清理已证明）且后续代成功时允许 quiesce 完成。建议 B，并补“失败→重试成功→协调 quiesce 完成”正反控。

## 11. 结果（逐例独立进程；`runs/<集合>/summary.txt` 每行“退出码 用例 XML计数”，`cases.txt` 为用例清单）

最终绿构建 = 快照产品与工作区本包文件逐一 `cmp` 一致（`hashes/snapshot-final-green-sha256.txt`），二进制 `hashes/final-green-binaries.sha256`；红构建 = 同一快照仅本包产品源换回 `before/`，二进制 `hashes/red-binaries.sha256`（已核对红二进制不含新产品诊断串）。红/绿均为对象增量编译 + 手工执行 ninja 记录的链接命令（避免重建与本包无关的 WtCore 静态库与 porter 动态库；依赖动态库为同一绿构建产物）。

| 集合 | 用例 | 绿（最终） | 红（before 产品） |
|---|---|---|---|
| wtcore 收窄目标全部 | 253（含 1 DISABLED） | 251 通过；1 失败 = R202003 RiskOn（WP1 产品待改，见第 6 节） | 15 非零：P025 ×7、P02Local ×2、R59 自动清理、R73 自动清理、R71 Preflight（3.1/3.4）、R71 Concurrent 超时 142（3.3）、R71 DiffV2 重试 exit 1（3.2）、R202003 RiskOn |
| exefact 全部 | 428（12 DISABLED） | 400 通过；16 失败，**与红完全相同**（既有：r7_twap ×8、r24_twap、r115_execmon、r220 ×3（1 例 SIGSEGV 139）、r24_runner、r7_vwap_profile、r7_vwap.unrepresentable…） | 26 非零 = 上述 16 + 本包新增 10（r17_order_identity 重复撤单 ACK/重复全成 ACK/localid 复用 3 例、TWAP 合法单位 5 例、TWAP 与 VWAP 锁仓清仓各 1 例；对照用例 reconnect、flat_clear、clear_target_dbl_max 红绿均通过） |
| btcore 收窄目标全部 | 89（8 DISABLED） | 63 通过；18 失败与红相同（R111ExecMocker ×16、R121、ExecMockerTest 1：快照未构建 exec_v2 夹具插件、回放器 init 失败，ExecMocker 未被本包修改） | 19 非零 = 上述 18 + R43 盘口簿快照用例 |
| NEW-014（MallocPreScribble/Scribble/GuardEdges） | 1 | 1/1 | — |
| NEW-053 最小复现与原用例对照 | 见第 5 节 | — | — |

关键单项（绿）：R71ExecTest.DiffV2FailedInitializationStillRequiresSafeAutomaticRetry、ConcurrentSameCode…、DiffFailedInitializationWaiters…、R71LocalSerialTest.Preflight…、P02LocalExecuterTest ×9、R10ExecuterTest ×19、R59Exec ×7、R73 ×4、R90 ×2、R15 ×11、P025AccountNetLimit ×7 全部 exit 0；R71ExecTest 18/18、R71LocalSerialTest 18/18、WtDiffExecuterConcurrencyTest 31/31、WtArbiExecuterSerialization 30/30、WtExecMgrQuiesceCoordinator 16/16、N02ExecuterFactoryTest 15/15、R7VwapV2LocalIntegration 8/8、r31_exe_fact 10/10、r17_diff_minimpact 全部、MatchEngine 相关 44 例通过。
未运行/未验证：ASan/TSan 本轮未跑（负载 100–200）；CoverageTests_execv2（旧插件拒绝/ABI 夹具）见第 13 节；Windows/MSVC、真实插件以外的供应商、整体回归均未执行。

## 12. 修改文件 sha256（前 12 位）

| 文件 | 改前 | 改后 |
|---|---|---|
| `src/WtCore/WtLocalExecuter.cpp` | 0bfd5e60d187 | 711f26463c4a |
| `src/WtCore/WtLocalExecuter.h` | fbfd5401e160 | 80f4bc549b9a |
| `src/WtCore/WtDiffExecuter.cpp` | 7ada114905d4 | 5d700afde5a9 |
| `src/WtCore/WtDiffExecuter.h` | bc39ab33c5c3 | 7405efcfe836 |
| `src/WtCore/WtArbiExecuter.cpp` | 409534fe27a3 | 461cc873745f |
| `src/WtCore/WtArbiExecuter.h` | 34a307dc3846 | 3c480ce379e3 |
| `src/WtCore/WtExecuterFactory.cpp` | 045f4198f0e0 | ecfac64bffd5 |
| `src/WtCore/WtExecuterFactory.h` | 95e8e176e690 | e116e238281b |
| `src/WtCore/AccountNetPositionLimits.h` | 新增 | 70976574d1d0 |
| `src/WtExeFact/WtTWapExeUnit.cpp` | d5a9209e9fc1 | 9304810a7d36 |
| `src/WtExeFact/WtVWapExeUnit.cpp` | 0cadadd1335a | 43ee6a3e5cba |
| `src/WtExeFact/WtOrdMon.cpp` | 81b4aab10dbc | 45e66344465c |
| `src/WtExeFact/WtOrdMon.h` | 09879e566544 | 254412cf1f1c |
| `src/WtExeFact/WtDiffMinImpactExeUnit.cpp` | 84afb8e2ce16 | e51c89e796f6 |
| `src/WtBtCore/MatchEngine.cpp` | 6cb6c95d32ff | cdec6cf493b0 |
| 测试 `p02_local_arbi_executer_test.cpp` | a7ca352b1e20 | 410df2adb747 |
| 测试 `r10_wtcore_executers.cpp` | 0f2689106e12 | 5f12e388e919 |
| 测试 `r59_exec_autoclear_test.cpp` | a61b0db760c3 | 20334d923651 |
| 测试 `r73_executers_test.cpp` | c0f4601a15f9 | 5f9092becce4 |
| 测试 `r71_executers_test.cpp` | 4fd85f965895 | 872a7150f2dc |
| 测试 `r71_local_serialization_test.cpp` | defad2c6b67b | 9c288685ba6f |
| 测试 `r90_executer_pool_test.cpp` | 9672d13f75d9 | 2b78213c2a4c |
| 测试 `r15_mgr_executers.cpp` | 5a5270325da6 | 0b420be93581 |
| 测试 `r43_match_engine_flow_test.cpp` | 89e588d1081f | 3cecf1b9510d |
| 测试 `r7_vwap_unit_test.cpp` | 2b0cfc9ee986 | 5a1483b7e090 |
| 新增 `r7_exefact/r17_order_identity_lifecycle_test.cpp` | — | 5d19ddaf9f2d |
| 新增 `r7_exefact/r7_twap_legal_lot_test.cpp` | — | e52a9875420f |
| 新增 `q01_wtcore3/p02_005_account_net_limit_test.cpp` | — | f259513b1ee5 |
| 新增 `q01_wtcore3/i03_new014_diff_config_init_test.cpp` | — | 5ec0e798f34a |
| 新增 `fixtures/i03_exec_units_20261104s5/r202_003_risk_unknown_code_test.cpp`（不进 lane） | — | fde9efe8d638 |

## 13. CoverageTests_execv2（旧插件拒绝 / V2 ABI 夹具回归）

同一绿构建（WtCore 静态库已在复原后的本包产品源上重编，日志含 WtExecuterFactory/WtLocalExecuter 目标），53 例逐例 53/53 exit 0（`runs/final-execv2/`）：V2ExecuterFactoryTest 20、V2ExecPluginQuiesceLifecycleTest 25、FactoryProbeFailures 3、TransientFailures 3、WtExecPluginV2InitFailure 2。覆盖旧 ABI/错误版本/短表/缺能力插件在旧工厂调用前拒绝、失败初始化即销毁不注册等，新增 `pluginEntered` 重载未改变这些行为与插件 ABI（`IExecPluginV2.h` 未修改）。

## 14. 各条目建议状态（主线程裁量）

| 条目 | 建议 | 依据 / 剩余 |
|---|---|---|
| BUG-I02-NEW-035 | 本地验证通过（本机范围） | Diff V2 安全重试与非阻塞 ready 修复后 R71 Local/Arbi/Diff 主组 18/18、串行组 18/18；红控 exit 94/超时 142。真实供应商、跨平台并发未验；Boost bridge 历史问题由本次实体快照重编覆盖 |
| BUG-R7-002 | 待验证 | V2 失败/成功后注册、旧插件安全拒绝路径未改动 ABI；Local/Arbi/Diff 真实 WtExeFact 宿主通过；回测 ExecMocker（R111/R121）在本快照因回放器 init 失败/夹具未建而未能复验，execv2 旧插件夹具 53/53（第 13 节）；Runner 生命周期与 Windows 未验 |
| BUG-R190-001 | 待验证 | 同 R7-002；账本并发落账备注已核对为不再成立（3.2）；D-2 待裁定 |
| BUG-R7-012 | 本地验证通过（本机 fake） | 卡E：买/卖/T+1/目标变化/重复行情/元数据不可用 5 例红→绿；真实市场零股规则未取得，按不允许处理 |
| BUG-R17-001 | 本地验证通过（单元层） | 重复 ACK、多订单、localid 复用、跨重连回收 4 例（3 红→绿、1 对照）；重复成交去重与跨重启身份属宿主 WP1（N05-003） |
| BUG-R43-002 | 待验证 | 旧档位清理已按快照语义实现并红→绿；锁存范围的跨重启/跨执行器/账户部分依赖卡5 WAL 与账户门禁（WP1） |
| BUG-R202-003 | 阻塞（待 WP1） | 合同明确（卡8c=A）；需 TraderAdapter 修改，合同用例已就绪且当前红 |
| BUG-P02-005 | 待验证 | 账户净额目标层门禁 7 例红→绿；D-1（配置位置、下单层持仓/在途检查）待裁定 |
| BUG-I02-NEW-053 | 本地验证通过 | 根因确证为覆盖率 profile 运行时（第 5 节）；R59Exec abort 已修复（7/7） |
| BUG-R7-004（Z） | 阻塞 + 部分修复 | 卡15 复核发现 TWAP/VWAP 清仓分支不可达的明确缺陷并修复（2 例红→绿）；其余（冻结/单向账户/股票卖空）按卡15“用户指示忽略”，解除条件：取得账户双向与冻结量接口合同 |
| I03-NEW-014 | 本地验证通过（malloc 填充代替 ASan） | 见第 8 节 |
