# Lane N03 交付清单 — WtUftCore / WtIndexFactory / WtLatency*

## 1. 新增测试文件（3 个，均在 `src/TestUnits/coverage/n03_uftcore_index/`）

| 文件 | 行数 | 用例数 | 说明 |
|---|---|---|---|
| `n03_idxfactory_worker_test.cpp` | 521 | 11 启用 + 1 DISABLED | WtIndexFactory 工厂注册/取用 + 板块指数 worker 纯计算 |
| `n03_action_policy_test.cpp` | 259 | 7 启用 + 1 DISABLED | ActionPolicyMgr 配置解析与规则组回退 |
| `n03_uftcore_helper_adapter_test.cpp` | 511 | 10 启用 + 1 DISABLED | WtHelper / UftDataDefs / TraderAdapter(+Mgr) / ParserAdapter(+Mgr) |

合计 28 条启用用例 + 3 条 DISABLED（崩溃类整体隔离），共 31 条（`grep -c '^TEST'` 实测 12/8/11）。
说明：两个文件略超简报建议的 400 行上限（测试夹具与假对象在同一 TU 内），如需可再按夹具拆分，本轮未拆以免破坏已过门禁的文件。

## 2. 覆盖的符号 / 分支

### `src/WtIndexFactory`（此前 0%）
- `WtIndexFactory.cpp`：`WtIdxFactory::get_name` / `create_worker`（命中 + 未注册名返回 NULL）/ `delete_worker`（正常、NULL、跨工厂拒绝）；`extern "C" create_index_factory` / `delete_index_factory`（含传 NULL）。
- `WtBlkIdxWorker.cpp`：`get_fact_name`；`init`（NULL 配置、`stand_scale` 缺省修正、`weight=0` 修正、`codes` 分支：合约存在/不存在、`sub_ticks` 返回基线快照/NULL、`commodities` 分支：品种不存在 continue + 品种存在但成分列表为空、`trigger="time"` 与 `trigger=<fullcode>` 两条 `extractStdCode` 路径）；`handle_quote`（非成分合约早退、`_trigger` 不匹配早退、`timeout==0` 直接出指数）；`generate_tick`（数据不全早退、`weight_alg` 0/1/2 三种权重算法的算式与总额累加、首帧建缓存 vs 后续 high/low 修正分支、`action_time` 单调性）。

### `src/WtUftCore`
- `ActionPolicyMgr.cpp`：`init`（文件不存在返回 false；order 数组解析、`wt_stricmp` 大小写不敏感映射 open/close/closetoday/closeyestoday、未识别动作跳过、`filters` 映射、无 order / 无 filters / 标量组、二次 init 追加语义）；`getActionRules`（显式组、未映射回退 default、重复查询稳定性）；`ActionRule` 默认构造与 `ActionType` 枚举取值。
- `WtHelper.cpp/.h`：`getCWD`、`getModulePath`（isCWD true/false）、`setGenerateDir` + `getBaseDir/getOutputDir/getStraDataDir/getStraUsrDatDir/getPortifolioDir`（目录自建与后缀校验）、`setTime/getDate/getTime/getSecs`、`setTDate/getTradingDate`、`setInstDir/getInstDir`。
- `UftDataDefs.h`：`BLK_FLAG`/`FLAG_SIZE`、`BlockHeader` 及 `PositionBlock/OrderBlock/TradeBlock/RoundBlock` 的 1 字节对齐 sizeof、`DetailStruct/OrderStruct/TradeStruct/RoundStruct` 的清零构造与字段读写。
- `TraderAdapter.h/.cpp`：`PosItem::total_pos/avail_pos`（多空 4 分支）、`RiskParams` 默认构造、构造函数/`id()`/`state()`、`init(NULL)`/`initExt(NULL api)` 拒绝、`run()` 无 api 返回 false、`release()` 无 api 空操作、`getOrders`/`cancel`/`cancelAll`/`getUndoneQty`/`enumPosition`（带参与带空串）/`getPosition`（flag 1/2/3 × bValidOnly）在空表下的安全分支、`addSink`；`TraderAdapterMgr::addAdapter`（NULL 指针、空名、重名拒绝、正常）、`getAdapter`（命中/未命中）、`getAdapters`、`run`、`release`。
- `ParserAdapter.h/.cpp`：`init(NULL)`、`init` 缺 `module` 字段的早退（避免加载动态库）、`getBaseDataMgr`、`id()`、`initExt(NULL api)` 拒绝、`initExt` 全流程（`registerSpi` → `api->init` → `getContracts()` → `subscribe`，含 `_initRet=false` 的错误日志分支）、`run()`（无 api / 有 api）、`handleQuote`（未停止转发 + `setCode` 改写为 fullcode、`quote==NULL`、`action_date==0`、基础数据查不到合约）、`handleParserLog`（正常与 `_stopped` 后丢弃）、`release()`（`_remover==NULL` 时 `delete _parser_api`）；`ParserAdapterMgr::addAdapter/getAdapter/run/release`。

## 3. 跳过目标及原因（供排除清单评审）

**需要 UFT 柜台 / 网络 / 共享内存 / DLL 装载，单测不可达（建议排除）**
- `WtUftCore/ShareManager.cpp/.h`（全量 507+585 行）：`initialize/start_watching/init_domain/set_value/get_value/allocate_value` 全部依赖 libwtpshare 共享内存段与 `func_*` 函数指针，无段即返回 false 或崩溃。
- `WtUftCore/UftStraContext.cpp/.h`（1358+889 行）：`watch_param/read_param/sync_param/commit_param_watcher` 走 ShareManager；`on_bar/on_tick/stra_*` 下单链路依赖 WtUftEngine + TraderAdapter + 真实委托回报。
- `WtUftCore/TraderAdapter.cpp` 的回报状态机（`handleEvent/onLoginResult/onRspPosition/onRspOrders/onRspTrades/onPushOrder/onPushTrade/onRspEntrust/buy/sell/openLong/openShort/closeLong/closeShort/doEntrust/doCancel`，约 L438–L1850）：需要可回调的 `ITraderApi` 与柜台回报序列、`_stat_map`/`_orders` 生命周期，建议用集成/回放层覆盖（另见 BUG-N03-005 使其在未初始化下不可达）。
- `WtUftCore/ParserAdapter.cpp` 的 `init` 动态库装载分支（`DLLHelper::load_library/get_symbol`）与 `handleOrderQueue/handleOrderDetail/handleTransaction`（需要 level2 的 `WTSOrdQueData/WTSOrdDtlData/WTSTransData` 池对象与 `_filters` 配置，本轮未构造）。
- `WtUftCore/EventNotifier.cpp/.h`（303+65 行）：`tradeToJson/orderToJson` 与全部 `notify*` 依赖 wtpmq DLL 与 ZeroMQ 服务端口；`boost::asio::post` 异步链路 + 未初始化 `_mqserver` 指针。`tradeToJson/orderToJson` 理论上是纯 JSON 拼装，但它们是私有成员且需先 `init` 成功（`init` 里 `DLLHelper::load_library`）。**建议主控评估把这两个函数提为可测的自由函数后再覆盖。**
- `WtUftCore/WtUftEngine.cpp/.h`、`WtUftDtMgr.cpp/.h`、`WtUftTicker.cpp/.h`、`UftStrategyMgr.cpp`（约 687+187+264+131 行）：引擎 `run()` 起线程 + 交易日历/共享内存；`UftStrategyMgr::loadFactories` 需要真实策略 DLL 目录。
- `WtIndexFactory/WtBlkIdxWorker.cpp:209-243`：`_timeout > 0` 时创建常驻 `_thrd_trigger` 线程并 `wait/notify`，且 `while(!_process)` 自旋等待，属挂死风险 + 墙钟依赖 → 不覆盖。
- `WtIndexFactory/WtBlkIdxWorker.cpp:83-104` 的 commodities 成分订阅完整链路：需要 `WTSCommodityInfo` 的成分代码集合（只由 `initFromVariant` 填充），本轮只能覆盖“品种存在但列表为空”与“品种不存在”两支。

**构建形态导致不可链接（请主控裁决）**
- `src/WtLatencyHFT`（506+98+34 行）与 `src/WtLatencyUFT`（327+91+34 行）：两者的 `CMakeLists.txt` 都是 `ADD_EXECUTABLE`（`src/WtLatencyHFT/CMakeLists.txt`、`src/WtLatencyUFT/CMakeLists.txt:42`），且各自带 `main()`；`HftLatencyTool.cpp/UftLatencyTool.cpp` 里的 `TestParser/TestTrader/TestStrategy` 是 .cpp 内的局部类，不在任何静态库/插件 TU 列表里，CoverageTests 无法链接（会把两个 `main` 也带进来）。
  这两个模块的可测纯逻辑其实只有 `getName/getFactName` 之类的常量返回和 `initModules/initEngine` 的环境装配（后者需要 `config.yaml`、行情/交易 DLL、共享内存）。**建议：要么把它们改造成 OBJECT 库 + 把装配函数参数化后由集成测试覆盖，要么整体列入排除清单。** 本轮按纪律未改任何 CMakeLists。

## 4. 待审核缺陷

`docs/迭代计划/迭代01/bugs/n03_uftcore_index.md`：8 条（BUG-N03-001 … BUG-N03-008），其中 Blocker 3 条（越界读 `WEIGHT_ALGS`、`getActionRules` 解引用 `end()`、裸代码下单路径 `buf[npos]` 越界写栈）。
崩溃/挂死类输入已整类收进 3 个 `DISABLED_` 用例（`DISABLED_n03_blkidx.weight_alg_out_of_range_class`、`DISABLED_n03_action_policy.no_default_group_deref_end_class`、`DISABLED_n03_trader_adapter.uninitialized_deref_class`），启用用例中不含任何已判定会崩溃/死循环的输入。
本轮未断言任何墙钟时间或本地时区；延迟统计类（WtLatency*）未覆盖，见第 3 节。

## 5. 门禁结果

按简报指定的唯一构建动作，对 3 个文件逐个执行
`clang++ -std=c++17 -fsyntax-only -Wall -Werror -Wextra -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY -include src/Share/fmt_enum_compat.h -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include <file>`
结果：**3/3 通过，0 error 0 warning**（最后一次全量门禁见本轮会话输出 `ALL_GATES_PASSED_IF_NO_ERRORS_ABOVE`）。
未运行 cmake/ninja/全量构建/测试二进制。
