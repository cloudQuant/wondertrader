# Lane p06 交付清单 — WtUftCore 剩余部分（测试覆盖率迭代01 第三波）

二进制归属：`CoverageTests_uftcore`（只编译 `src/WtUftCore/*.cpp`）。
未引用任何 `WtCore`/`WtBtCore`/`WtDtCore` 类，未改任何 `CMakeLists.txt`，未改 `src/**` 生产代码。

## 1. 新增测试文件（3 个，`src/TestUnits/coverage/p06_uftcore2/`）

| 文件 | 行数 | 用例数 | 说明 |
|---|---|---|---|
| `p06_uft_engine_test.cpp` | 453 | 10 启用 + 1 DISABLED | WtUftEngine 状态查询/时间设置/基础数据委派/订阅分发/上下文表 + WtUftDtMgr + WtUftRtTicker |
| `p06_uft_context_test.cpp` | 269 | 6 启用 + 1 DISABLED | UftStraContext 不依赖共享内存的回调与查询分支 |
| `p06_stra_mgr_test.cpp` | 212 | 7 启用 | UftStrategyMgr 注册表存取与参数校验 + ShareManager 单例装配 |

合计 23 条启用用例 + 2 条 DISABLED（`grep -c '^TEST'` 实测 11/7/7 = 25）。
`p06_uft_engine_test.cpp` 超出简报建议的 400 行上限 53 行（同 TU 内放假基础数据管理器与引擎夹具，拆分会破坏「一个夹具一份状态」的先写后读约束）。
落盘只发生在 `std::filesystem::temp_directory_path()/p06_<tag>_<pid>_<seq>`，RAII 析构 `remove_all` 清理。

## 2. 覆盖的符号 / 分支

### `src/WtUftCore/WtUftEngine.cpp/.h`（此前 0%）
- 构造函数（`_tm_ticker/_cfg/_notifier` 初始化 + `TimeUtils::getDateTime` + `WtHelper::setTime` 调用链）、析构（`_cfg`/`_tm_ticker` 均为 NULL 的分支）。
- `init`（`cfg == NULL` 时不 retain 的分支；`_base_data_mgr/_data_mgr/_notifier` 装配）；`get_basedata_mgr`。
- `set_date_time`（`rawTime == 0` 用 curTime、显式 rawTime 两支）、`set_trading_date`、`get_date/get_min_time/get_raw_time/get_secs/get_trading_date`。
- `get_commodity_info`（合约不存在早退、合约存在但 `getCommInfo()` 为 NULL 的透传支）、`get_contract_info`（命中/未命中）、`get_session_info`（`isCode=false` 直查、`isCode=true` 合约不存在早退）。
- `get_last_tick`（空表 NULL）、`get_cur_price`（NULL tick → 0.0、命中 → 取价并 release、同合约二次推送更新、未知合约 0.0）。
- `get_tick_slice`/`get_kline_slice` 的 `return NULL` 死代码首行（见 BUG-P06-005）、`get_order_queue_slice`/`get_order_detail_slice`/`get_transaction_slice`。
- `sub_tick`/`sub_order_detail`/`sub_order_queue`/`sub_transaction`（新建订阅项 + 同 sid 重复订阅去重）。
- `on_tick`（`_data_mgr` 为空早退、订阅表命中+上下文命中、命中订阅但上下文缺失、订阅表 miss）、`on_bar`（空订阅表两支）、`on_minute_end`。
- `handle_push_quote`（`_tm_ticker == NULL` 早退）、`handle_push_order_detail`/`handle_push_order_queue`/`handle_push_transaction`（三张表各自的命中与 miss）。
- `addContext`/`getContext`（空表 miss、命中、不同 id 追加、缺失 id）、`on_session_begin`/`on_session_end`、`on_init`、`notify_params_update`（名称命中 + 全不命中）。

### `src/WtUftCore/WtUftDtMgr.cpp/.h`（此前 0%）
- 构造函数（三指针置 NULL）、`init`（返回 true）、`handle_push_quote`（`newTick == NULL` 早退、`_rt_tick_map` 懒创建、`_ticks_cache == NULL` 早退）、`grab_last_tick`（map 为 NULL、命中）、`get_tick_slice`/`get_kline_slice`/`get_order_queue_slice`/`get_order_detail_slice`/`get_transaction_slice` 空实现、析构（`_rt_tick_map` 非空的释放支）。

### `src/WtUftCore/WtUftTicker.cpp/.h`（此前 0%）
- 构造（正常引擎 / NULL 引擎）、析构（未起线程）、`init`（`getSession` 返回 NULL → `_s_info == NULL` 支）、`on_tick` 的 `_thrd == NULL` 转发支（含 `_engine` 判空两支）。

### `src/WtUftCore/UftStraContext.cpp/.h`（此前 0%）
- 构造（`IUftStraCtx(name)`/`_engine`/`_tradingday`/`_strategy`）与析构、`makeUftCtxId` 自增、`id()`/`name()`、`set_strategy`/`get_stragety`、`setTrader`。
- `on_init`、`on_tick`（持仓表 miss + 策略为空）、`on_bar`、`on_order_queue`、`on_order_detail`、`on_transaction`、`on_session_begin`、`on_session_end`、`on_params_updated` 的策略为空支。
- `stra_get_date`/`stra_get_time`/`stra_get_secs`/`stra_get_price`（0.0 与命中价、未知合约）、`stra_get_local_position`/`stra_get_local_posprofit`/`stra_get_local_closeprofit` 的 miss 支（含空串）、`stra_sub_ticks`/`stra_sub_order_details`/`stra_sub_order_queues`/`stra_sub_transactions`、`stra_log_info`/`stra_log_debug`/`stra_log_error`（含空消息）。

### `src/WtUftCore/UftStrategyMgr.cpp/.h`（此前 0%）
- `loadFactories`：目录不存在 → false；目录存在且为空 → true；目录含子目录/无后缀/`.so` 后缀（macOS 只认 `.dylib`）→ 全部 continue 跳过，不触发任何 `DLLHelper::load_library`；二次装载幂等。
- `createStrategy(factname, unitname, id)` 工厂未注册支；`createStrategy(name, id)` 的参数校验（无 `.`、空串、只有点、点分名工厂缺失）；`getStrategy` miss 支；空注册表析构。

### `src/WtUftCore/ShareManager.h`
- `self()` 单例地址一致性、`set_engine()` 纯赋值（多次改写）。其余全部未覆盖，见第 3 节。

## 3. 跳过目标及原因（供排除清单评审）

**需要 UFT 柜台 / 共享内存 / DLL / 历史数据服务，单测不可达（建议排除）**
- `WtUftEngine::run()`：`new WtUftRtTicker` + `init(session)` + `_tm_ticker->run()` 起线程并 join，属简报明令禁止的 `run()/start()`；`_cfg->has("product")` 配置解析支只能在此处进入（引擎其余配置解析仅此 6 行）。
- `WtUftRtTicker::run()/stop()` 与 `on_tick` 的 `_thrd != NULL` 全部时间推算支（`timeToMinutes/minuteToTime/isLastOfSection/on_minute_end/_next_check_time`）：需要常驻线程 + `sleep_for` + 墙钟，属挂死风险类（本轮 5 条 HANG 教训）。
- `UftStraContext` 的 `watch_param`/`read_param`/`sync_param`/`commit_param_watcher`（`ShareManager::self().allocate_value(...)`，无共享内存段即空函数指针）；下单链路 `stra_buy/stra_sell/stra_enter_long/stra_enter_short/stra_exit_long/stra_exit_short/stra_cancel/stra_cancel_all`；回报 `on_trade/on_order/on_position/on_entrust/on_channel_ready/on_channel_lost`（需 `ITraderApi` 回报序列 + `is_my_order` 的 `_orders` 生命周期）；`stra_get_bars/stra_get_ticks/stra_get_order_detail/stra_get_order_queue/stra_get_transaction/stra_get_last_tick`（需行情历史库）。
- `UftStraContext::load_local_data()` 与 `position.membin/order.membin/trade.membin/round.membin` 的 mmap 建块、块头 `_blk_flag/_date/_capacity` 写入、跨交易日 `memset` 回收（`UftStraContext.cpp:1031-1358`，本轮实测约 330 行未覆盖）：需要 `WtHelper::getOutputDir()` 真实可写目录 + `BoostFile/BoostMappingFile` 映射 + `mannual.yaml` 走 `WTSCfgLoader`（`WTSUtils`，不在 `CoverageTests_uftcore` 的编译集合内）。**建议主控把「块存取与回收」单列为一个可注入文件根的函数后再覆盖。**
- `ShareManager.cpp` 全部（`initialize/start_watching/init_domain/set_value/get_value/allocate_value` + `func_*` 装载与 `sleep_for` 轮询线程）与 `ShareManager.h` 的 `set_value/get_value/allocate_value` 内联体（同上，上一波已列入排除）。
- `UftStrategyMgr::loadFactories` 的 DLL 循环体（`DLLHelper::load_library/get_symbol("createStrategyFact")/creator()/free_library` 三支）与 `createStrategy` 命中工厂后的 `UftStraWrapper` 构造、`_strategies` 写入、`getStrategy` 命中支：需要真实策略插件目录。
- `TraderAdapter.cpp` 的 `getRiskParams/checkCancelLimits/checkOrderLimits/getInfos/isTradeEnabled/updateUndone/printPosition/getContract`：`getRiskParams` 与 `getInfos` 等入口无条件解引用 `_stat_map`/`_bd_mgr`，而这些容器指针只由 `init()/initExt()` 装配；`init` 需要 UFT 柜台 DLL，`initExt(NULL api)` 又被早退拒绝 → 单测内无法把适配器推进到可用状态。本轮未启用任何相关断言，并按纪律把「fresh adapter 访问器」整体留在 DISABLED（与 BUG-N03-005、上一波 `n03_trader_adapter.fresh_adapter_public_accessors` 段错误同一缺陷类）。
- `EventNotifier.cpp/.h`、`ParserAdapter.cpp` 的 `init` 动态库装载与 L2 处理链、`WtHelper`/`UftDataDefs`/`ActionPolicyMgr`：上一波已覆盖或已排除，本波未重复。

## 4. 待审核缺陷

`docs/迭代计划/迭代01/bugs/p06_uftcore2.md`：**6 条（BUG-P06-001 … BUG-P06-006），状态一律待审核**，其中 Blocker 3 条：
- BUG-P06-001 `WtUftEngine` 构造未初始化 `_base_data_mgr/_data_mgr/_dependent`（野指针）
- BUG-P06-002 非标准化代码缺 `.` 时 `ay[1]` 越界读（3 个查询接口）
- BUG-P06-004 `UftStraContext::_trader` 未初始化 + 8 个下单/查询接口无判空
- BUG-P06-003 `get_session_info(code,true)` 空 `commInfo` 解引用（Major）
- BUG-P06-005 `get_tick_slice/get_kline_slice` 首行 `return NULL` 造成约 40 行永久死代码并令 `on_bar` 分发失效（Major，阻塞覆盖率）
- BUG-P06-006 `WtUftRtTicker::init/on_tick` 对 `_engine/_s_info` 判空不一致（Major，需 `run()` 才能进入）

会崩溃/越界的输入已整类收进 2 个 `DISABLED_` 用例（`DISABLED_p06_uft_engine.uninitialized_and_malformed_stdcode_class`、`DISABLED_p06_uft_context.uninitialized_trader_and_param_class`），启用用例不含任何已判定会崩溃/死循环/阻塞的输入；未调用任何 `run()/start()/join()`、未连接任何网络或柜台、未断言墙钟与本地时区。

## 5. 门禁结果

按简报指定的唯一构建动作，对 3 个文件逐个执行（并在最后对全目录再跑一遍确认当前落盘状态）：
```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include <file>
```
结果：**3/3 通过，0 error 0 warning**（实测：`p06_stra_mgr_test.cpp` / `p06_uft_context_test.cpp` / `p06_uft_engine_test.cpp` 均 OK，错误行数为 0）。
未运行 cmake/ninja/全量构建/测试二进制。

## 6. 预算与剩余

- 本轮工具调用超出 30 次预算（约 36 次），主要消耗在 `WTSDataDef.hpp` 的 `create()` 重载与 `TraderAdapter` 风控函数入口的确认上。
- 因预算收口而**未做**的候选目标（下一波可直接接手，均已定位到文件）：
  1. `UftStraContext::load_local_data()` 的 mmap 块建/回收（约 330 行，需要先解决 `WTSCfgLoader` 依赖或注入文件根，见第 3 节）。
  2. `TraderAdapter` 风控与合约校验纯函数（需先给出一个不依赖柜台 DLL 的 `initExt` 可用装配路径）。
  3. `WtUftDtMgr` 与 `WtUftEngine` 的历史缓存（bar/tick cache）路径。
