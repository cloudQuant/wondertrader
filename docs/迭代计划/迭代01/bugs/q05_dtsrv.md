# q05_dtsrv 缺陷清单（迭代01 第五波 · WtDtCore/WtDtServo/WtDtHelper）

- 范围: 本波仅记录，未修改任何生产代码。
- 关联用例目录: `src/TestUnits/coverage/q05_dtsrv/`

## BUG-Q05-001 WtDtServo::WtDataManager::initStore 模块加载失败后空指针解引用
- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtDtServo/WtDataManager.cpp:59-88`
- 现象: `DLLHelper::load_library` 失败（或插件缺 `createRdmDtReader`/`deleteRdmDtReader` 符号）时 `_reader` 保持 NULL，但 L87 无条件执行 `_reader->init(cfg, this)`，直接段错误。`init()` L104 又无条件调用 `initStore(cfg->get("store"))`，即「配置里 store.module 写错 / 插件没装」这一最常见部署故障路径必崩，而不是返回 false。
- 对照: 兄弟实现 `src/WtDtCore/DataManager.cpp:67-107` 同形状代码在加载失败时 `return false`，且 `_writer->init` 在守卫之后——Servo 版疑似移植时丢了这个 return。
- 复现: `q05_servo_wtdatamgr_test.cpp` 的 `DISABLED_Q05ServoWtDataManagerTest.DISABLED_InitStoreLoadFailureDerefsNullReader`（按硬约束保持禁用，启用即段错误）。

## BUG-Q05-002 CoverageTests_dtsrv 目标内 ParserAdapter/ParserAdapterMgr 强符号重复
- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtDtCore/ParserAdapter.{h,cpp}` 与 `src/WtDtServo/ParserAdapter.{h,cpp}`
- 现象: 两个全局类**同名且多数方法签名逐字相同**（`~ParserAdapter()`、`init(const char*, WTSVariant*)`、`initExt`、`release`、`run`、`handleQuote(WTSTickData*, uint32_t)`、`handleSymbolList(const WTSArray*)`、`handleOrderQueue/handleOrderDetail/handleTransaction/handleParserLog/getBaseDataMgr`，以及 `ParserAdapterMgr::release/run/getAdapter/addAdapter`），两份 .cpp 都编进 `CoverageTests_dtsrv` 后为同签名强符号，链接期必 duplicate symbol（macOS ld 直接报错）；即便按 BUG-L00-005 思路摘掉一份，另一份的归因也是假的。两个头文件还各以 `#pragma once` 定义不同类形状的 `class ParserAdapter`，任何 TU 各自包含一份即构成 ODR UB。
- 影响: 本波因此**未对两个 ParserAdapter 写任何启用用例**（原计划靶子「行情过滤与时间戳判定」无法在该目标下安全归属）；主控需要先把其中一份按目录级编译别名（同 CMakeLists 里 WtStraFact 家族的做法 `PARSER...=..._dtcore`）拆开，或把 WtDtServo 的 ParserAdapter TU 移出本目标，再补波次。
- 建议: 长期方案是产品侧给 WtDtServo 版加命名空间；测试侧不改生产代码。

## BUG-Q05-003 IndexWorkerMgr::handle_quote 同步分支 retain 后不 release（行情对象泄漏）
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/IndexWorkerMgr.cpp:170-203`
- 现象: L180 `newTick->retain()` 之后，仅 `_pool` 异步分支在任务末尾 `newTick->release()`；`_pool==NULL` 的同步分支遍历完 `_workers` 就直接返回，引用计数只加不还。datakit 以「poolsize=0 + 高频 tick」运行时每 tick 泄漏一个 WTSTickData 池对象，池/内存单调增长。
- 佐证: 两分支唯一差异就是这句 release；语义上 handle_quote 对入参引用计数应平衡。
- 复现: `q05_idxworker_mgr_test.cpp::HandleQuoteNullUnsubscribedAndSubscribed`（同步分支路径本身已被启用用例走过，断言引用计数需暴露池内部状态，未做）。

## BUG-Q05-004 resample_bars 对 cb/cbCnt 无 NULL 守卫（cbLogger 有）
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtHelper/WtDtHelper.cpp:1021-1023`
- 现象: 同函数内 12 处 `cbLogger` 调用全部先判空，而成功路径 `cbCnt(newCnt); cb(&kline->getDataRef().at(0), newCnt, true);` 裸调用；头文件签名默认参数只给 `cbLogger` 提供 `= NULL` 缺省（cb/cbCnt 无缺省），一旦 C 侧调用方传 NULL 即空指针解引用。另外 `kline->getDataRef().at(0)` 在 `newCnt==0` 时越界访问 vector 首元素。
- 复现: 未写启用用例（属崩溃类输入，与 BUG-Q05-001 不同类，暂不建 DISABLED 条目，交排除清单评审）。

## BUG-Q05-005 StateMonitor::initialize 的配置泄漏与会话 ID 缓冲区溢出风险
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtCore/StateMonitor.cpp:49-74`、`src/WtDtCore/StateMonitor.h:47`
- 现象: ① L49 `WTSCfgLoader::load_from_file` 返回的 `config` 全程未 `release()`，每次初始化泄漏整棵配置树；② `StateInfo::_session` 为 `char[16]`，L74 用 `strcpy` 拷贝任意长度的 JSON key，`sid` ≥16 字符即栈溢出 UB（配置可外源输入，属不安全拷贝）。另 `WTSBaseDataMgr::loadSessions`（`src/WTSTools/WTSBaseDataMgr.cpp:282-284`）对缺 `sections` 的条目 `continue` 前已 `new` 出 `WTSSessionInfo` 且不释放，同类泄漏。
- 备注: ②的溢出路径属崩溃类输入，未收录任何启用用例；本波 DISABLED_ 名额用于 BUG-Q05-001（同文件内崩溃类不叠加）。

## 待裁定（期望值推导不充分，允许首跑失败后由主控裁定）
1. `q05_resample_bars_test.cpp::DayResampleOfTenDailyBarsPassesThrough` 断言 `d→d(times=1)` 全区间透传 10 条：由 L961-1010 的 lower_bound 区间推导 hitCnt=10，但 `WTSDataFactory::extractKlineData(KP_DAY, 1)` 是否恒返回 NULL 或改写条数依赖 WTSTools 实现，未在本波预算内读其源码。若实测失败请裁定为「实现缺陷」或「约定如此」并调整断言。
2. 同文件 `MinuteResampleReportsConsistentCounts` 只断言自一致性（返回值==cbCnt 参数==cb 条数），不断言 m5 合并具体条数——合并条数取决于 `extractKlineData` 的会话对齐语义，推不出唯一期望值。
3. `q05_state_monitor_test.cpp::InitializeMalformedJsonReturnsFalse` 假设 `load_from_json` 对坏 JSON 返回 NULL（`WTSCfgLoader.cpp:323` 起未再核对 parse 失败分支），若返回空对象而非 NULL 则本用例语义变为「空配置」，需改写。
