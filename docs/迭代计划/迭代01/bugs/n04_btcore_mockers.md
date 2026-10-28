# N04 WtBtCore 模拟撮合/数据管理缺陷记录（迭代 01）

状态一律「待审核」；按 `bugs/README.md` 模板，测试与修复提交分离，未修改任何生产代码。
同类缺陷若上一轮（L06）已记录，本文件只引用编号，不重复造条目。

## BUG-N04-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtBtCore/ExecMocker.cpp `handle_trade` L334、`handle_order` L311、`handle_entrust` L287、`handle_init` L155、`handle_schedule` L189；`init` L49-86
- 相关测试: src/TestUnits/coverage/n04_btcore_mockers/n04_exec_mocker_test.cpp `DISABLED_matchSinkCallbacksCrashWithoutExecuterUnit`
- 输入/前置条件: `ExecMocker` 已构造但 `init()` 未成功（执行器 DLL 缺失/装载失败），因此私有成员 `_exec_unit` 仍为未初始化指针（构造函数初始化列表未包含 `_exec_unit`、`_volunit`、`_target`、`_sig_time`）
- 期望行为: 回报回调应先判空 `_exec_unit`（未装载执行单元时丢弃回报或明确报错），`init` 失败后对象仍应处于可安全调用状态
- 实际行为: 五个入口无条件 `_exec_unit->on_trade/on_order/on_entrust/on_channel_ready/set_position` → 空指针/未初始化指针解引用崩溃；`handle_trade`/`handle_order` 还无条件解引用 `_last_tick`（未喂 tick 时为 NULL）；`handle_init` 在 `_period` 为空串时读取 `_period[0]`（UB）
- 复现步骤: `ExecMocker m(&replayer); m.handle_entrust(1,"SSE.600000",true,"ok",20240102093000000ULL);` 即 SIGSEGV
- 影响范围: 执行单元装载失败（配置错误/模块缺失）时回测进程直接崩溃，无法给出可读错误；同时导致 ExecMocker 的成交/撤单回写（`_position`、`_undone`、`_trade_logs`）无法在单测中覆盖
- 备注: 阻塞本 lane 对 ExecMocker 成交回写分支的覆盖率目标。建议修复方向：构造函数补齐 `_exec_unit(NULL)/_volunit(0)/_target(0)/_sig_time(0)`，回调入口判空；不实施。

## BUG-N04-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtBtCore/HisDataMgr.cpp `init` L36-74（缺陷落在 L71 `_reader->init(cfg, this)`）
- 相关测试: src/TestUnits/coverage/n04_btcore_mockers/n04_hisdatamgr_test.cpp `DISABLED_initCrashesWhenReaderModuleMissing`
- 输入/前置条件: ①空配置对象（`module` 缺省 → 拼装 `安装目录 + wrap_module("WtDataStorage")`，本机无该 .so/.dylib）；②显式指定不存在的模块路径；③`createBtDtReader` 符号缺失的库；④`init(nullptr)`
- 期望行为: 模块装载或符号缺失时应记录错误并 `return false`，`_reader` 保持 NULL，由 `load_raw_*` 的判空分支兜底
- 实际行为: 两个失败分支都只写日志（且符号缺失分支写日志后继续向下），随后无条件执行 `_reader->init(cfg, this)`，`_reader` 为 NULL → 空指针解引用崩溃；`init(nullptr)` 直接对空 `this` 调用 `cfg->getCString`
- 复现步骤: `HisDataMgr m; WTSVariant* c = WTSVariant::createObject(); m.init(c);` → SIGSEGV
- 影响范围: 数据存储模块缺失/改名时回测启动即崩溃（HisDataMgr 全部 5 个 `load_raw_*` 的判空保护形同虚设）
- 备注: 阻塞 HisDataMgr 主路径（reader 成功装载后的 `read_raw_*` 转发与回调触发）覆盖，单测内无法注入 DLL；建议 `if (_reader == NULL) return false;`。

## BUG-N04-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtBtCore/CtaMocker.cpp 空态查询族（`stra_get_price` L1504、`stra_get_day_price`、`stra_get_fund_data` L1885、`do_set_position` L1603、`append_signal` L1586、`proc_tick` L695、`update_dyn_profit` L973、`get_cond_entrusts` L1297）与 `init_cta_factory` L415
- 相关测试: src/TestUnits/coverage/n04_btcore_mockers/n04_cta_mocker_state_test.cpp `DISABLED_emptyStateCallsCrashOrHang`
- 输入/前置条件: `CtaMocker` 仅构造（`HisDataReplayer` 空态：无品种信息、无 tick、无策略），未走 `handle_init/handle_session_begin` 回放链路
- 期望行为: 空态下查询接口应返回中性值（0/NULL/空串），下单类接口应校验 `get_commodity_info()` 后拒绝并告警
- 实际行为: 上述接口依赖 `WTSCommodityInfo*`/`WTSTickData*` 非空与回放线程配对（`step_calc` 等待 calc done 通知），空态调用会崩溃或在 `_mtx_calc` 条件变量上挂死；`init_cta_factory(nullptr)` 解引用空配置
- 复现步骤: 见禁用用例；单条 `stra_get_price("SSE.600000")` 在未订阅行情时即进入未保护分支
- 影响范围: 无法在单进程「一条用例一个进程」模式下覆盖 CtaMocker 的持仓命中/信号/条件委托分支（CtaMocker 2228 行主体仍为 0 覆盖），需集成回测任务或为这些接口补充判空
- 备注: 不新建重复条目——`ExecMocker::cancel(stdCode, isBuy, qty)` 忽略 `stdCode` 与按量撤单数量语义错误，属上一轮已记录的 BUG-L06-003（`MatchEngine::cancel` 同一实现），此处仅引用。
