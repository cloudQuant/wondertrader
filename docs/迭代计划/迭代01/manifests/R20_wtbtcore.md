# R20 用例清单 — WtBtCore（覆盖率 lane R20）

- 派生时间：2026-10-28，lane R20 收口
- 范围：`src/WtBtCore/` 剩余未覆盖 ~341 行（口径 `manifests/api-brief-R20.md`）：
  `ExecMocker.cpp` 113/230、`HisDataMgr.cpp` 52/90、`EventNotifier.cpp` 44/64、四个 `Mocker.h` 内联段各 ~20
- 用例目录：`src/TestUnits/coverage/p08_btcore_mockers3/`（已登记目录，**未新建目录、未改 CMakeLists**），
  文件名前缀 `r20_`，映射目标 `CoverageTests_btcore`
- 族约束：只引用 `WtBtCore/`、`Includes/`、`Share/`、`WTSTools`；未触碰 WtCore/WtUftCore/WtDtCore 同名 `wtp::` 类
  （37 个同名类跨族会静默择一）
- 语法门禁：`cd build-cov && ninja -t commands
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/p08_uft_order_position_test.cpp.o | tail -1`
  取整行 flags，剥去 `-MD/-MF/-MT/-o <obj> -c <src>`，改 `-fsyntax-only <本文件>` 逐文件执行。
  **结果：4/4 文件通过（0 error / 0 warning，含 -Werror）**（详细见文末命令）
- 用例总数：**33（28 启用 + 5 `DISABLED_`，全部为崩溃/真实 socket 路径）**
- 生产代码：未修改；`src/**` 除 `p08_btcore_mockers3/` 外只读
- 硬规则自检：无 `#define private public`（本目录前人 p08 用法未沿用）、无新线程/sleep/真实 socket/真实 shm、
  不读挂钟（getCurTime 全部由 replayer 零值字段推导）、`SelMocker::handle_replay_done` 除零沿用 q02 整类
  DISABLED 未复活、未重新声明产品 TU 内 inline/static 符号（`makeLocalOrderID` 沿用库内定义，测试只经
  ExecMocker::buy 间接触发）
- 落盘：仅 `temp_directory_path()/r20_exec_mocker_<pid>/exec/`，TearDown `remove_all`；`dist/**` 未读未写
- Bug 记录：`docs/迭代计划/迭代01/bugs/R20_wtbtcore.md`，**8 条**（S2×2 / S3×4 / S4×1 / S5×1），
  状态均「待审核」，未修源码

## 用例 → 生产文件:行段

### 1. `r20_event_notifier_test.cpp`（6 启用 + 2 DISABLED）
| 用例 | 生产文件:行段 |
|---|---|
| CtorLeavesPublisherNullOnStaticInstance | EventNotifier.cpp L41-45（ctor）、L124-129（notifyEvent 守卫空转；static 存储期实例的 `_publisher` 零初始化是**本 lane 唯一确定性入口**，见 bugs S2） |
| NotifyDataDoesNotTouchBufferWithoutPublisher | L139-144（notifyData 空转，自喂哨兵 buffer 断言未被改写） |
| NotifyFundBuildsJsonAndNoopsWithoutPublisher | L157-185（rapidjson Document/PrettyWriter 拼装全段 + 发布守卫） |
| InitReturnsFalseWhenInactive | L66-70（active=false 早退） |
| InactiveInitIsSideEffectFreeOnFreshInstance | 同上 + 独立 static 实例互不污染 |
| FailedInitLeavesServerIdZero | L52-57（dtor `_remover && _mq_sid!=0` 短路可观察不变量） |
| DISABLED_InitActiveLoadsMqModuleAndOpensServer | L73-115 整段：load 成功后 `_creator(url,true)` 起**真实 MQ 服务**（本机 build-cov 存在 libWtMsgQue.dylib，违反禁 socket 规则）；L96 `free_library` 臂一并留白 |
| DISABLED_InitNullConfigDerefsAndCrashes | L69 cfg NULL 崩溃（bugs S3） |

### 2. `r20_hisdatamgr_test.cpp`（6 启用 + 1 DISABLED）
| 用例 | 生产文件:行段 |
|---|---|
| LoadRawBarsRefusesWithoutReader | L85-92（`_reader==NULL` 守卫 + LL_ERROR + return false，回调 0 次） |
| LoadRawTicksRefusesWithoutReader | L113-120 |
| LoadRawTransAndOrdqueRefuseWithoutReader | L141-148、L169-176 |
| LoadRawOrdDtlRefusesWithoutReader | L197-204（含空参重复调用） |
| AllFiveGuardsAreIndependentAndRepeatable | 五守卫混排，无状态累积 |
| ReaderLogPassesEveryLevelToGlobalLogger | L24-28（reader_log→WTSLogger::log_raw 全级别透传） |
| DISABLED_InitCrashesWhenReaderNullAfterLoadFailure | L36-74 全部：两失败臂不 return，L71 NULL `_reader` 解引用必崩（bugs S2） |

### 3. `r20_exec_mocker_test.cpp`（13 启用 + 2 DISABLED）
| 用例 | 生产文件:行段 |
|---|---|
| DefaultsAfterCtor | L28-40（ctor 零值）、L88-96/L204-217（getCommodityInfo/getSessionInfo/getPosition/getOrders/getUndoneQty 查无臂；SetUp 自检三段式 `SHFE.rb.2405` 查 comminfo 为 NULL） |
| CurTimeDerivedFromReplayerClock | L98-101（getCurTime 与 replayer 三字段自洽，不读挂钟） |
| TickSwapReleasesPreviousTick | L43-47（dtor release）、L122-137（handle_tick 换入换出 + 旧 tick release 臂；`WTSTickData::create` 公开池工厂=R2 实测接缝；`_exec_unit==NULL` 跳过 L135-136） |
| BuySellRefusedBeforeAnyTick | L219-224/L236-241（match engine 无 tick → ret 空 → 计数臂不进）、L253-257（cancel 未知 id → change 0 → false） |
| BuyAfterTickAccumulatesUndone | L221-233（buy 成功臂：_ord_cnt/_ord_qty/_undone += (int32_t)qty） |
| SellDecrementsUndone | L238-250（sell 臂 _undone -= qty；三档排队价：对手价/买一价/最新价，最新价臂分母 3799*5+3801*7 非零） |
| CancelByIdRestoresUndoneAndDoubleCancelQuirk | L259-264（归还+撤单计数）+ **重复撤单二次扣减现行为固化**（bugs S3，MatchEngine::cancel(localid) 不判 state/不 erase） |
| CancelByCodeSkipsUnfiredOrders | L267-278（state!=1 continue → 空 ret、回调不触发） |
| SessionEndClearsMatcherAndUndone | L113-120（_matcher.clear + _undone=0 + 统计日志；清表后 cancel 查无） |
| NoopEventsAndWriteLogAreSideEffectFree | L103-111（handle_bar_close/session_begin 空体）、L280-283（writeLog→log_dyn_raw("executer", 空id)） |
| InitFailsFastWhenModuleMissing | L49-62（getCString/getDouble/getInt32 读参 + regisSink + MatchEngine::init(NULL) 早退 + load_library NULL → false） |
| ReplayDoneWritesEmptyTradeCsv | L337-347（getOutputDir+`exec/`+`trades_.csv` 落盘、幂等重写；_id 空 → 文件名 `trades_.csv`，_trade_logs 未写表头 → 0 字节） |
| GetTicksNullWhenTickFeedDisabled | L194-197（委托 get_tick_slice 的 `!_tick_enabled` 首守卫，replayer 公开 `enable_tick(false)` 确定性进入） |
| DISABLED_ExecUnitNullDerefFamily | L139-161（handle_init）、L163-192（handle_schedule，_last_tick/_exec_unit 双裸）、L285-335（handle_entrust/order/trade） |
| DISABLED_FireOrdersSinkNullAfterPendingOrderAndSecondTick | tick→buy→tick 序列触发 MatchEngine::fire_orders NULL `_sink`（regisSink 只在 init 内） |

### 4. `r20_mocker_inline_id_test.cpp`（3 启用）
| 用例 | 生产文件:行段 |
|---|---|
| CtaMockerIdInlinePassesThroughContextId | CtaMocker.h L334（内联 `id(){return _context_id;}`，两实例 id 互异且稳定） |
| SelMockerIdInlineDistinctFromCta | SelMocker.h L254（同形内联体） |
| UftZeroPositionAccessorsAgreeOnEmptyState | UftMocker.h L329-331 内联访问器经公开零持仓查询族（stra_get_position iFlag 1/2/3、local、undone、date/time/secs）补锚一次空表臂 |

## 有意留白（接缝/规则挡住，估计行数）

1. `ExecMocker.cpp` `_exec_unit` 成交链与 `handle_schedule`（≈55 行）：ExecuteUnit* 无公开注入接缝
   （ExecMocker.h 全 private + 禁 `#define private public`），需要真实 executer 插件 DLL；
   `grabLastTick` 委托（2 行）依赖 `checkTicks` 的 bin 文件臂（未 init replayer、无 loader 夹具，不硬闯）。
2. `HisDataMgr.cpp` 五臂 reader 非空路径（≈25 行）+ `init` 正/负全程（≈24 行）：`_reader` private 无 setter；
   init 成功臂需真实 `WtDataStorage` 插件、失败臂必崩（已 DISABLED+S2 缺陷）。合计留 ~39 行。
3. `EventNotifier.cpp` `init(active=true)` 全支（≈30 行）：load 成功即 `_creator(url,true)` 起真实 MQ
   socket，禁；`free_library`/符号缺失臂同封。
4. 四个 `Mocker.h` 内联段剩余（≈78 行）：CtaMocker.h `log_signal/log_trade/log_close/get_cond_entrusts`
   与 SelMocker.h 同名内联的**函数体在 .cpp**（头文件行是声明），头文件内真实联体是 PosInfo/DetailInfo/
   PosItem 私有嵌套结构访问器，无公开接缝可达；UftMocker.h `frozen()/closeprofit()/dynprofit()` 需真实
   成交链（T1 冻结组合），本轮无 tick 缓存 loader 夹具未做。R2 已确认可用的
   `init(cfg, notifier, IBtDataLoader*)` 接缝若要复现需重建 loader fixture，超出本 lane 预算。
   本轮不碰 R2 标注「别硬闯」的 level2 `cacheRawOrd*FromBin`+`getNext*Time`、`replayHftDatas*`/`run_by_ticks`、
   主力连续 `cacheIntegratedFutBarsFromBin`。

## 验证命令（逐文件执行模板，结果见文首「语法门禁」行）

```
cd build-cov
ninja -t commands \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/p08_uft_order_position_test.cpp.o \
  | tail -1
# 在本目录任一已入库 cpp 的整行 flags 上：剥去 -MD / -MF <d> / -MT <o> / -o <o> -c <src>，
# 注意行内含带引号的 "-Wno-#pragma-messages"，需按 shell 规则分词（本轮用 python shlex.split），
# 再追加 -fsyntax-only <本文件绝对路径> 逐文件执行。
```

实测记录：`r20_event_notifier_test.cpp` 首跑因未 `USING_NS_WTP;` 报 6 处 `WTSVariant` 未限定名，
补一行后 exit 0；其余 3 文件首跑即 exit 0。四文件最终 **exit=0/0/0/0**（含 -Werror，0 warning）。

链接依赖说明：测试引用 `WtBtCore` 的 `ExecMocker/MatchEngine/HisDataMgr/HisDataReplayer/EventNotifier/WtHelper`
符号与 `WTSTools::WTSLogger`、`WTSBaseDataMgr`、`WTSVariant`、`makeLocalOrderID`（库内 HftMocker.cpp 提供，
测试未重复定义）；需主控把本目录 4 个 `r20_*.cpp` 纳入 `CoverageTests_btcore`（目录 glob 重 configure 即收）。
