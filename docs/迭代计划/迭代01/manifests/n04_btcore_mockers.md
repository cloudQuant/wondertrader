# N04 WtBtCore 模拟撮合与数据管理 交付清单（迭代 01）

- Lane: N04，模块 `src/WtBtCore/`（范围：`CtaMocker`/`SelMocker`/`UftMocker`/`ExecMocker`/`HisDataMgr`）
- 产出目录: `src/TestUnits/coverage/n04_btcore_mockers/`
- 语法门禁: **3/3 文件通过**（0 error 0 warning，Apple clang / macOS arm64，命令见文末）
- 用例总数: 23 个 `TEST_F`，其中 20 个启用 + 3 个 `DISABLED_`（缺陷类整体收纳，见 bugs）
- 生产代码: 未修改（`src/WtBtCore/**`、所有 `CMakeLists.txt` 只读）；未向仓库写入任何运行期产物
- Bug 记录: `docs/迭代计划/迭代01/bugs/n04_btcore_mockers.md`，共 3 条，状态均「待审核」；撤单方向过滤缺陷引用 `BUG-L06-003` 未重复造条目
- 排除项: `MatchEngine`、`HftMocker::splitVolume`、`HisDataReplayer` 时段/费率/运行期接口、`WtHelper`/`EventNotifier` 已由 l06 与本 lane 历史测试覆盖，本轮未触碰这些文件

## 新增测试文件与覆盖符号

### 1. `n04_exec_mocker_test.cpp`（244 行，10 个用例）
覆盖 `ExecMocker`（`src/WtBtCore/ExecMocker.{h,cpp}`）中与执行器 DLL 无关的分支：
- 构造/析构默认值：`getPosition`（validOnly/flag 三种组合）、`getUndoneQty`、`getOrders`（恒 NULL）、`grabLastTick`
- `init`：空配置对象、`module` 指向不存在路径 → 均返回 false，且配置字段（code/period/volunit/volmode）解析后状态未被污染；`_matcher.regisSink/init(cfg->get("matcher")==NULL)` 路径
- `ExecuteContext` 查询：`getCommodityInfo`/`getSessionInfo`（未知合约 → NULL）、`getTicks`（空/非空一致性，不假定缓存策略）、`getCurTime`（与 `TimeUtils::makeTime(get_date, get_raw_time*1e5+get_secs)` 公式一致且幂等；不断言墙钟）
- 委托链路：`handle_tick`（被动前喂 tick、重复喂入替换 `_last_tick`）→ `buy`（无缓存 tick 返回空；有 tick 时 1 笔被动单、`_undone += qty`）→ `cancel(uint32_t)`（置撤单态、`_undone` 归零、重复撤单返回 false）→ `sell`（`_undone` 取负）→ `cancel(stdCode,isBuy,qty)`（方向过滤 + 回调回写）
- `handle_session_end`（`_matcher.clear()` + `_undone=0` + 统计日志）、`handle_bar_close`/`handle_session_begin`（空实现）、`writeLog`
- `DISABLED_matchSinkCallbacksCrashWithoutExecuterUnit`：BUG-N04-001 缺陷类全体（`handle_entrust`/`handle_order`/`handle_trade`/`handle_init`/`handle_schedule`/`init(nullptr)`）

### 2. `n04_hisdatamgr_test.cpp`（170 行，6 个用例）
覆盖 `HisDataMgr`（`src/WtBtCore/HisDataMgr.{h,cpp}`）+ init 的路径拼装：
- 未初始化（`_reader==NULL`）时 `load_raw_bars`（KP_Minute1/KP_Minute5/KP_DAY）、`load_raw_ticks`、`load_raw_ordque`、`load_raw_orddtl`、`load_raw_trans` 全部返回 false 且回调 0 次命中；空 `FuncLoadDataCallback` 同样早退；空交易所/空合约/日期 0 与重复调用的幂等
- `reader_log` 六个日志级别转发（未初始化日志器走 `print_message` 分支）
- 文件/模块定位：`WtHelper::setInstDir/getInstDir` 往返、`DLLHelper::wrap_module("WtDataStorage")` 与 `安装目录+wrap_module` 的拼装稳定性（`init` 内同一公式）、与 `WtDataStorageCsv` 的区分
- `init` 的配置读取语义：`getCString("module")` 缺省返回 `""`（走默认模块名分支）、显式模块名原样读取
- 临时目录使用 `std::filesystem::temp_directory_path()/n04_hisdatamgr_inst`，TearDown `remove_all`
- `DISABLED_initCrashesWhenReaderModuleMissing`：BUG-N04-002 缺陷类全体（空配置/不存在模块/符号缺失/`init(nullptr)`）

### 3. `n04_cta_mocker_state_test.cpp`（151 行，7 个用例）
覆盖 `CtaMocker`（`src/WtBtCore/CtaMocker.{h,cpp}`）空态查询与上下文分配：
- 构造（含 slippage/persistData/notifier/isRatioSlp 全参重载）与 `id()`：`makeCtxId` 进程级原子自增的单调性与互异性
- `_pos_map`/`_sig_map` 未命中分支：`stra_get_position`（默认/`bOnlyValid`/带 userTag/空合约）、`stra_get_position_avgpx`、`stra_get_position_profit`、`stra_get_detail_entertime`、`stra_get_detail_cost`、`stra_get_detail_profit`（flag 0/1/2）
- 时钟透传：`stra_get_tdate`/`stra_get_date`/`stra_get_time` 与 `HisDataReplayer::get_trading_date/get_date/get_min_time` 一致（不断言绝对时间/时区）
- 空行情：`stra_get_comminfo`/`stra_get_last_tick` → NULL、`stra_get_rawcode` → 空串
- `stra_log_info/debug/warn/error` 未初始化日志器安全路径 + 查询无副作用
- `DISABLED_emptyStateCallsCrashOrHang`：BUG-N04-003 缺陷类全体

## 跳过目标及原因

- **`SelMocker`（1174+837 行）与 `UftMocker`（1321+370 行）**：本 lane 30 次工具调用预算内未能完成读取+编写（剩余预算优先用于三个文件的门禁验证与缺陷/清单交付）。已完成的准备：两者方法行号索引已抽取（`UftMocker::stra_get_position` L1013、`stra_get_local_position` L1024、`stra_get_price` L1049、`update_position` L1155、`init_uft_factory` L130；`SelMocker` 的 `dump_*`/`init_sel_factory` 与 `stra_get_*` 族在 .h L100-L564），结构与 CtaMocker 同构（持仓 map 私有、成交回写依赖撮合链路），建议下一轮按 `n04_cta_mocker_state_test.cpp` 的模板复用。**注意**：`UftMocker.h` L329-L339 的 `valid()/volume()/frozen()/closeprofit()` 内联与 `SelMocker.h` L698 `valid()` 是纯函数，可脱离回放环境直接测（是下一轮最省成本的切入点）。
- **`CtaMocker` 的 `dump_outputs`/`dump_stradata`/`dump_chartdata`/`do_set_position`/`append_signal`/`proc_tick`/条件委托**：私有成员（编译期即报 `'do_set_position' is a private member of 'CtaMocker'`），且命中分支需要 `WTSCommodityInfo` + tick 事件线程配对，单进程内不可构造；已记入 BUG-N04-003。
- **`ExecMocker::handle_init/handle_schedule/handle_replay_done` 与成交回写（`_position`/`_trade_logs`）**：依赖 `_exec_unit`（需真实执行器 DLL）与输出目录约定，见 BUG-N04-001。
- **`HisDataMgr::load_raw_*` 的成功分支**：需要 `createBtDtReader` 动态库装载成功，仓库内无只读样例可驱动（l06 清单亦记为跳过，此处未重复）。
- 未使用随机数；未断言墙钟/本地时区；用例之间不共享状态（每文件独立 fixture，`WtHelper::setInstDir` 在 TearDown 复位）。

## 验证命令（逐文件已执行，3/3 clean）

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/n04_btcore_mockers/<file>.cpp
```

链接说明：测试引用 `HisDataReplayer`、`MatchEngine`、`makeLocalOrderID`（由 `WtBtCore` STATIC 库内 `HftMocker.cpp` 提供）与 `WTSLogger`/`WtHelper`，需由主控在 `CoverageTests` 目标中链接 `WtBtCore`；本 lane 未修改任何 `CMakeLists.txt`，若 coverage 子目录不是自动 glob 收集，需主控把 `src/TestUnits/coverage/n04_btcore_mockers/` 三个文件加入源列表。
