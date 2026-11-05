# s5-runners：Runner / Porter / ExecMon 入口（迭代03，2026-11-04）

macOS arm64，HEAD `ccaad010`（工作区含大量他人未提交改动，见 `hashes/HEAD.txt`）。本目录只记录本包定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实供应商或最终整合树。未修改任何跟踪文件；需他包改动与决策卡草稿见下文与 `decision-cards.md`。

负责条目：BUG-R134-003、BUG-WSN06-001、BUG-WSN06-002（E）、BUG-R13-004、BUG-R14-010、BUG-R28-005、BUG-R104-003、BUG-R42-002（E）；Z 类：BUG-R6-006、BUG-R134-004、BUG-I02-NEW-025、BUG-I02-NEW-123；主线程追加 I03-NEW-045、I03-NEW-046。

## 构建与快照

- 共享源码树由约 12 个子任务并发修改，直接在活树上增量构建时 ninja 反复整树重编（他人头文件变化），且第一次构建（`/tmp/wt-i03-s5-runners-cov`，“build1”）编译时混入了本包尚未完成的中间版本，**build1 结果只作早期诊断，不计证据**（`runs/early_*` 已标注）。
- 证据构建：`rsync -a --no-links` 将活树 `src/` 与 `dist/common/` 复制为物理快照 `/tmp/wt-i03-s5-runners-snap/src`（`find -type l` 为空；文件哈希 `hashes/snapshot-sha256.txt`），之后只把本包拥有的文件逐个同步进快照（`cmp` 校验与活树一致）。配置命令即前置规则中的默认覆盖率命令（`-Werror` 默认开启），构建目录 `/tmp/wt-i03-s5-runners-snap/build`。本包产品文件在 `-Werror` 下编译通过（WtPorter/WtExecMon/WtUftStraFact/WtRunner 产品库与覆盖率目标）。
- 红控：`scripts/build_red_overlay.py` 复制快照为 `/tmp/wt-i03-s5-runners-red/src`，用 `before/` 中的改前产品文件覆盖（WtRtRunner.cpp、WtPorter.cpp、WtExecRunner.cpp、两个 demo、WtRunner main/cpp/h；纯增量的 WtRtRunner.h/WtPorter.h/ExpHftContext.h 保留新版以保证与绿色对象布局一致），只用绿色构建的原样编译命令重编这些 TU 及直接 include 了产品 .cpp 的测试 TU，再用绿色链接命令替换对象重链（只替换实际链入该可执行体的对象；`i03_s5_r42_capi_v2_test.cpp.o` 依赖新导出，红控链接时剔除，属静态红控）。红色可执行体复制到绿色 `bin/TestUnits/` 下以 `_red` 后缀运行（部分用例按可执行体目录查找模块）。
- 运行：`scripts/run_suites.sh` / `scripts/runcase.sh`，每个 suite 独立进程、独立 cwd（`/tmp/wt-i03-s5-runners-run/<label>`）、`perl alarm` 硬超时、独立 `LLVM_PROFILE_FILE`；需要存储模块的 WtRunner 用例以 `RUNCASE_STAGE` 把 `libWtDataStorage.dylib` 放入 cwd（与主线程 r104 结论一致）。XML/日志/退出码在 `runs/`。

【结果与条目结论见下文各节】

## 产品修改（改前/改后 sha256 前 12 位见 `hashes/before-after-sha256.txt`，差异见 `patches/`）

| 文件 | 内容 | 关联 |
|---|---|---|
| `src/WtPorter/WtRtRunner.cpp` | ① config 纯预检：executers（内联或侧文件，侧文件只读一次并复用）全表校验（local/diff/arbi 用各自 `validateConfig`，dist 放行，未知 kind、重复 id、非对象拒绝），SEL 模式校验 `strategies.sel`；失败释放 `_config` 保持可重试。② `initExecuters` 加载工厂前全表预检；执行器按引擎（SEL/CTA）登记，**先登记后 init**（失败宿主由管理器持有、release 时 quiesce，不再泄漏），trader sink 延后到全部成功后统一 `addSink`；返回值与 router 加载结果均传播为配置失败。③ SEL 路由：`_is_sel` 走新的 `initSelStrategies`（读 `strategies.sel`，创建→全部 init→发布三段；未知工厂/init false/异常均整批失败、无 context）；CTA/HFT 策略 init(false)/异常传播且全部成功后才发布。④ `createExtExecuter` 按引擎登记并拒绝空/重复 id；`createSelContext` 拒绝空参数/未知周期。⑤ `run` 启动期异常（如策略 on_init 抛出）fail-closed：调用 `release()` 而不是标 RUNNING。⑥ I03-NEW-045 类：`completeRelease` 中仍持有 API 的 parser owner（无排空证明）移入进程级保留容器；析构遇失败 parser 候选同样保留后完成 teardown，其余未完成情形仍 `std::terminate`。⑦ I03-NEW-046：析构（进程退出期 porter 单例）先关闭全部 31 个宿主回调入口，再 release。⑧ I03-NEW-064：`createCta/Sel/HftContext` 只在 runner 已配置的同类引擎上创建。 | R134-003、WSN06-001/002、NEW-025、R13-004（周期部分）、NEW-045/046/064 |
| `src/WtPorter/WtRtRunner.h` | 新增 `_host_callbacks_closed`（仅 porter 内部类布局） | NEW-046 |
| `src/WtPorter/WtPorter.h/.cpp`、`ExpHftContext.h` | 新增导出 `hft_get_position_v2`/`hft_get_undone_v2`（状态码 OK/INVALID_ARGUMENT/UNKNOWN_CONTEXT/TRADER_UNBOUND，失败输出 NaN），`ExpHftContext::isTraderBound()`（非虚，内联）；旧导出不变；`register_ext_data_loader` 对非空 tick loader 告警并在头文件写明不支持；`create_sel_context` 文档改为实际口径并标注待裁定 | R42-002、R134-004、R13-004 |
| `src/WtExecMon/WtExecRunner.cpp` | `initExecuters` 全表预检（local/diff/dist，未知 kind 拒绝）；parser owner 无排空证明时保留到进程退出，析构不再因此 terminate | R134-003、WSN06-001、NEW-045 类 |
| `src/WtRunner/main.cpp`、`WtRunner.h/.cpp` | runner 改为 `unique_ptr` 并标 `final`；`main` 在控制线程有限重试 `shutdown()`，仅剩无排空证明的 parser owner 时输出诊断并刻意保留 runner 到进程退出（不 abort，退出码不变）；新增 `parserOwnersRetained()`；析构其余失败仍 fail-stop 并加诊断 | I03-NEW-045 |
| `src/WtUftRunner/main.cpp`、`WtUftRunner.h/.cpp` | 同上原则 | I03-NEW-058 |
| `src/WtHftStraFact/WtHftStraDemo.cpp`、`src/WtUftStraFact/WtUftStraDemo.cpp` | 参数严格 schema：未知键拒绝；HFT 必填 `code`，UFT 必填 `code` 与正数 `lots`（原缺省变 0 手）；数值/布尔按文本严格解析（YAML 标量为字符串）；发行配置中的遗留键（HFT `count`，UFT `count/level2/maxloss`）接受并忽略 | WSN06-002 |

`-Werror` 默认配置下上述产品与全部覆盖率目标编译通过（`logs/build-final*.log`）。

## 测试修改与新增（逐个 .before 在 `before/`）

- 新增：`q06_porter/i03_s5_rt_runner_preflight_test.cpp`（19 例）、`i03_s5_r42_capi_v2_test.cpp`（2）、`i03_s5_hft_demo_params_test.cpp`（2）、`i03_s5_test_trader_support.cpp`（无模块 TraderAdapter 登记、按引擎直接安装导出上下文、按引擎重建并配置进程级 porter 的辅助）；`m_WtUftRunner/i03_s5_uft_demo_params_test.cpp`（2）；`r24_exefact2/i03_s5_exec_runner_preflight_test.cpp`（4）；`m_WtRunner/r104…`新增 `bound_executers_then_failed_diff_init_release_cleanly`（真实 TraderMocker + local/diff 绑定后 diff init 失败，释放与析构回归）；夹具 `fixtures/i03_runner_quarantine_exit_20261104s5/`（真实 WtRunner/WtUftRunner 进程入口 + 无法证明排空的 V2 fake parser）。
- 按已定合同更新旧前提（均在注释中写明卡号）：R42-002 门禁（以不存在 trader 建 HFT 上下文的 v2 三例、R124×3、R13 forward、R163、R143、R214、R134、R46 expctx、q06 ctx table、r46 config，改为登记无模块 TraderAdapter 或断言返回 0）；卡8b=A（r46/r82/r124/r163/r220 porter、r24 r220 exec、m_WtRunner r220 执行器补 policy；R69/R82 缺失/坏侧文件与 `routers:{}` 改为失败或数组）；卡2 2b=A（全天交易时段 `0-2400`→`0-0`：v2、zz_r220、新增夹具、r95、r78）；卡13d/I03-NEW-008 费用门禁（r28、r6 指向有效空模板）；卡9c（R82 C API 用例切换 env 时重建进程级 porter）；I03-NEW-064（R13 cta/sel export 先按引擎配置；R35、R46 expctx、R13 forward、q06 ctx table 等只验转发的夹具改为直接安装上下文）；WSN06-002（R82 有效 SimpleHft 给出最小合法 params）；R124 HFT 改用只读上下文账本的均价断言（同 r214 先例）；uftcore 建议补丁 `SUGGESTED_not_applied_m_WtUftRunner_r220.diff` 已审阅应用（确认型 remover，卡9a）。
- `fixtures/i03_exec_eager_validation_20261103/sel_factory.cpp` 补卡4b V2 getter（只编译验证，未重跑该夹具的整树快照脚本）。

## 运行结果（快照构建；红=改前产品文件覆盖重链）

- **porter 全 lane（73 suite/274 例含 disabled，`runs/final/final_porter*`）**：绿色失败 7（修 R46 后），均与本包改动无关或待裁定：`ParserRelease*`×2（发现 E/NEW-123，决策卡）、`ExternalReleaseDoesNotWaitForInFlightProviderSnapshot`（套件内计时竞态，单例 3/3 通过，红色亦间歇失败）、`R134PorterBars`×3 与 `R69…ValidDataStoreModuleLoadsAndServesKlineQuery`（`get_bars` 返回空，红绿相同，存储读取链问题，交主线程分派）。红色额外失败 18 例 = 本包新增用例的红控（预检 5/6、SEL 4/4、策略 2/2、启动异常 1/2、引擎作用域 1/1、HFT demo 1/2）+ 按新合同更新的旧用例 + `zz_r220…external_parser_quote_is_drained_before_parser_map_release`（改前因隔离 parser 留在 map 中失败，改后通过）；`I03S5R42HftQuantity` C API v2 两例依赖新导出（静态红控）。
- **exefact**：`I03S5ExecRunnerPreflight` 绿 4/4、红 3/4 失败（合法正控通过）；`r220_exec_runner_trader_binding` 补 policy 后红绿均 4/4。
- **m_WtUftRunner**（`final_uft_summary.txt`）：本包 demo 2/2（红 1/2：9 种畸形参数被旧 demo 接受）；uftcore 补丁后 `WtUftRunnerParserOwnership` 2/2、`WtUftRunnerParserStart` 逐例 3/3、整 suite 复跑 3/3（首次整 suite 运行在第 3 例挂起被杀，记间歇）；r95/r85/r14/r78 其余全过；`r95_uft_parser.successful_fake_parser_registers_runs_and_quiesces_before_release` 失败：其 `R130ParserLifecycle` 夹具无 V2 getter，被卡9 后的 UFT ParserAdapter 拒绝（“lacks lifecycle V2”），需共享夹具补 V2（该夹具也被 wtcore 使用，未改）。
- **m_WtRunner**（cwd 放置存储模块；`final2_wtrunner_summary.txt`）：r104 9 例（含新增）全过；R28-005 原定向 router 用例全过（N02 五例另用主线程 `-Werror` 无链接二进制复跑 5/5，`runs/r28-n02`，二进制哈希在内）；r6/r220/r28_strategies（后者不放置存储模块时）通过。剩余失败均为他包合同变更导致的前提失效：`WtRunnerParserInitFailure`（旧版无 V2 fake parser 被卡9 拒绝）、`R28ParserCommands.engine_exception…`（initExt parser 按卡9 隔离，测试随后析构 runner → terminate；与本包改动无关，需重设计为保留 runner）、`r28…router_nul_alias…`/`embedded_nul_file…`（配置加载器现拒绝含 NUL 内容）、`router_sidecar_uses_preflight_snapshot…`（单例 1/1；整 suite 内日志已初始化为 error 级别）。
- **I03-NEW-045/058 进程入口**（`runs/q45_runner_exit`）：改前 WtRunner、WtUftRunner 均 SIGABRT（134，`libc++abi: terminating`）；改后退出码 0，日志各 1 行“retained until process exit”。
- **R14-010**：`fixtures/r14_runner_startup_overlap/run.sh` 以当前 WtUftRunner 源码（与改前哈希一致）重跑，普通/ASan+UBSan/TSan 各 12/12（`runs/r14-overlap/results.txt`）；随后为 NEW-058 修改了 WtUftRunner 与 r95 夹具，该夹具的快照一致性检查将拒绝重跑，需主线程按新源码重建快照后复跑。
- 未运行：主目标 `CoverageTests`（n06 WSN06 用例）、`CoverageTests_selfthft`/btcore p08 中 SimpleHft/SimpleUft 用例（静态核对其参数均满足新 schema）、sanitizer、Windows、真实 provider；整体回归 NOT_RUN。

## 条目结论

- **BUG-R134-003**：建议**本地验证通过**（WtPorter/WtRtRunner、WtExecMon 均已接入 runner 层全表预检与 SEL 路由；红绿见上）。残留：失败批中已创建的 SEL/CTA/HFT 策略对象由各 StrategyMgr 缓存至 runner 析构（无移除接口，需 engine-lifecycle 提供 `removeStrategy`），策略 init 外部副作用无通用回滚；真实插件/Windows 未验。
- **BUG-WSN06-001**：建议**本地验证通过**（Z 类最终复核发现的缺陷已修：SEL 路由、init 传播、启动异常 fail-closed、执行器泄漏）。重复 run 被拒（`SecondRunIsRejectedAfterSuccessfulStart`，R10-004 只报告）；CTA/SEL ticker 线程内 on_init 异常隔离属引擎。
- **BUG-WSN06-002（E）**：建议**本地验证通过**（HFT/UFT demo 严格 schema 红绿；runner 侧 init(false) 传播）。合同：未知键拒绝、必填缺失拒绝、发行配置遗留键接受；其他第三方策略的 schema 由各策略自定，未覆盖。
- **BUG-R42-002（E）**：建议**本地验证通过**（门禁：未知/空 trader 返回 0、无残留；新增 v2 版本化查询区分未知句柄/未绑定/非法参数）。需 engine-lifecycle：`HftStraBaseCtx::stra_get_position/stra_get_undone` 对 `_trader==NULL` 仍直接解引用（经 porter/runner 已不可达，直接构造上下文的宿主可达），建议返回 NaN 并记日志。
- **BUG-R13-004**：**阻塞**。已做：未知周期/空参数拒绝、SEL 配置预检、文档改为实际口径。待裁定见 `decision-cards.md` 草稿 R13-A；需 engine-lifecycle 修 `WtSelEngine::on_minute_end`：`getContext(tInfo->_id)` 用任务 ID 查 context（应为 map key 即 context id）、`StdThreadPtr` 局部 joinable 线程析构必 `std::terminate`（任一 SEL 任务触发即进程中止，静态确认）、月度日期比较 `_cur_date % 1000000` 与回测 `%100` 不一致、`min` 周期先比较 `_time==nextTime` 再取模语义冲突。
- **BUG-R14-010**：建议**本地验证通过**（重复/并发 run、启动线程内与回调内 shutdown：夹具 36/36 + lane 内 ParserStart/SinkShutdown 用例通过；NEW-058 进程入口红绿）。真实 provider 排空为 X。
- **BUG-R28-005**：建议**本地验证通过**（无链接实体快照重编，原定向 router 用例全过，N02 5/5）；活跃 executor quiesce 内部状态、Windows 未验。
- **BUG-R104-003**：建议**本地验证通过**（无链接重编，r104 9/9 含真实 TraderMocker + local/diff 绑定后失败释放回归）；未连接真实交易通道。
- **Z 类**：NEW-025 **建议本地验证通过**（未知 SEL 工厂受控失败、无部分 context，红控 1/1 失败→绿）；R6-006 **建议用户指示忽略**（缺 env 失败、显式 store 失败已传播；缺 data 段与 no-store 模式合同仍冲突，复核发现缺 data 时外部数据加载器不登记，无崩溃）；R134-004 **建议用户指示忽略**（IHisDataLoader 无 tick 入口、无调用者，已改为非空 tick loader 告警+文档写明不支持）；NEW-123 **阻塞**（随草稿 E/NEW-123 裁定；现状两例失败为前提失效）。
- **I03-NEW-045/058**：红绿闭合（见上）；**I03-NEW-046**：静态（析构关闭宿主回调；initExt API 本就不被 release）；**I03-NEW-064**：红绿闭合（`I03S5RtRunnerEngineScope` 红失败→绿）。

## 新发现（编号由主线程分配）

1. `WtHftRtTicker::run` 在会话缺失（`_s_info==NULL`）时直接解引用崩溃（`WtHftTicker.cpp` `_s_info->id()`）；CTA/SEL ticker 同型需核对。引擎应在 run 前拒绝。复现：HFT 配置使用被卡2拒绝的 `0-2400` 交易时段后 `run(true)`（build1 lldb 栈见 early 记录）。
2. `R134PorterBars`×3、`R69…ValidDataStoreModuleLoadsAndServesKlineQuery`：真实 WtDataStorage 读回 0 根 K 线，红绿相同（非本包）。
3. `WtRtRunner::completeRelease` 原先在 `_parsers.release()` 后直接销毁仍持 API 的 parser（无排空证明）——本轮已按卡9a 保留（见上），列出供主线程确认归口。
4. WtExecRunner 静态单例析构遇失败 parser 候选会 terminate（与 NEW-045 同类），本轮已一并按保留原则修改，未做运行红绿（静态）。

## 需他包改动

- engine-lifecycle：`Cta/Sel/HftStrategyMgr` 提供失败批策略移除接口；`WtSelEngine::on_minute_end` 四处缺陷（见 R13-004）；`HftStraBaseCtx` 未绑定数量查询空指针；ticker 空会话崩溃（新发现 1）。
- WtCore ParserAdapter / 卡9：initExt 停止通知合同（草稿 E/NEW-123）。
- 共享夹具：`fixtures/r130_parser_lifecycle.cpp` 需补 V2（m_WtUftRunner r95_uft_parser、wtcore 共用）；m_WtRunner `n01_parser_init_failure_test` 的 fake parser 需 V2；`R28ParserCommands` 需按卡9a 保留 runner；r28 两个 NUL 用例需按新加载器合同改写。

## 删除说明

只删除过本包自建路径：`/tmp/wt-i03-s5-runners-snap`（自建快照，重建一次）、`/tmp/wt-i03-s5-runners-run/<label>`（runcase 每次重建）、`/tmp/wt-i03-s5-runners-red/CoverageTests_porter*`（自建红色对象）。未删除任何他人目录。
