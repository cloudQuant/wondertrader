# P03 WtBtCore 模拟交易主路径 交付清单（迭代 01 第三波）

- Lane: p03，模块 `src/WtBtCore/`（CtaMocker/SelMocker/UftMocker 的下单—成交—持仓—盈亏推进 + 持仓结构内联语义）
- 产出目录: `src/TestUnits/coverage/p03_btcore_mockers2/`，映射 `CoverageTests_btcore`（仅引用 WtBtCore/Includes/Share/WTSTools 头，未触碰 WtCore/WtUftCore/WtDtCore，规避 BUG-L00-005 强符号归因）
- 语法门禁: **3/3 文件通过**（0 error 0 warning，Apple clang macOS arm64，命令见文末；最后一处纯注释修改未复跑）
- 用例总数: 20（19 启用 + 1 `DISABLED_`，空解引用缺陷类整体收纳）
- 生产代码: 未修改；`CMakeLists.txt` 未动；落盘仅 `temp_directory_path()/p03_*` 唯一目录且 TearDown `remove_all`；`dist/common/**` 未读未写
- Bug 记录: `docs/迭代计划/迭代01/bugs/p03_btcore_mockers2.md` 3 条，状态均「待审核」

## 新增文件与覆盖符号

### 1. `p03_pos_structs_test.cpp`（11 用例）
n04 指定的最省成本切入点——三 Mocker 头文件内联结构（经 `#define private/protected public` 取得可见性，不改布局）：
- `CtaMocker.h` L837-L931: `_DetailInfo`/`_PosInfo`（含 `valid()` L896）/`_SigInfo` 默认值与 memset 语义、valid 多/空/全冻结/脏数据
- `SelMocker.h` L680-L736: `PosInfo::valid()` L698、`SigInfo._triggered` 默认 false
- `UftMocker.h` L289-L360: `PosItem::valid()/volume()/frozen()` L329-L331、`PosInfo::closeprofit()/dynprofit()` L339-L340、`DetailInfo/StraFundInfo` memset；avail>vol 脏数据下 `frozen()` 为负的边界

### 2. `p03_cta_trade_path_test.cpp`（6 用例，1 DISABLED）
`CtaMocker.cpp` 交易主路径，固定时钟（`HisDataReplayer::_cur_*`）+ 手工注册 `WTSCommodityInfo`（`_bd_mgr._fullpid_map`）+ `_fee_map` 按量费模板驱动：
- 未知合约拒绝：`stra_enter_long/short/stra_set_position` 早退（L1305/L1350/L1520）
- 开仓链：`stra_enter_long -> append_signal`（gentime/sigprice/sigtype=L1586）`-> proc_tick -> do_set_position`（L1603）成交明细（价格/量/opentime/opentdate/opentag）
- 盈亏推进：`update_dyn_profit`（L973）浮盈/最大浮亏/最高最低价；`stra_get_position_avgpx`
- **平今/平昨费用归属**：同 tdate 平仓 offset=2（平今费 3.0/手）、跨日 offset=1（平昨费 2.0/手），`_fund_info._total_fees/_total_profit/_total_closeprofit` 确定值断言
- 固定滑点双向修正（买入+2跳/卖出-2跳，L1637/L1674）、全平清零、反手做空
- 股票 T+1：`_frozen` 冻结、`stra_get_position(onlyValid)`、低于冻结拒绝、禁做空拒绝、等目标早退（L1520-L1551）
- 条件单：限价 `WCT_SmallerOrEqual` 登记 `_condtions` 与越价触发（L723-L824）
- `DISABLED_CalcFeeByAmountCrashesWhenCommodityMissing`：BUG-P03-001/002 缺陷类全体

### 3. `p03_sel_trade_path_test.cpp`（3 用例）
`SelMocker.cpp` 选股推进（T+1 股票 + 双向 ETF）：
- `stra_set_position`（L591）守卫族：未知合约/目标相等/禁做空/低于冻结拒绝；`SigInfo._triggered` 非调度即触发
- `append_signal -> proc_tick -> do_set_position`（L646）：开仓费、`stra_get_last_entertime/last_enterprice` 透传、浮盈推进、减仓 FIFO 归属、`_dynprofit` 等比缩放（5->4）、全平清零、`_last_exittime`
- ETF 多翻空：先平后反手两条明细、空单浮盈、费用累计（L646 反手分支）

## 跳过/剩余
- `UftMocker.cpp` 的 `update_position/on_trade/stra_buy` 推进：依赖 `_uft_ctx`、hook 与任务队列线程，启用用例不可安全驱动（挂死风险），本轮仅覆盖其头文件内联语义；建议后续与 n03 UftCore 一并处理
- `CtaMocker::handle_tick/on_tick_updated` 真实 tick 事件与 `MatchEngine` 联动：l06/n04 领域，未触碰
- `stra_exit_long`（依赖 `WTSSessionInfo::offsetTime/getCloseTime` 非空会话）：未注册会话时解引用风险，未在启用用例调用；已在 bugs「其它观察」记录与 `stra_exit_short` 的总量/valid 不对称
- 未用随机数；未断言墙钟/时区；用例间零共享状态

## 验证命令（逐文件执行）

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/p03_btcore_mockers2/<file>.cpp
```

链接说明：测试引用 `HisDataReplayer`、`WTSBaseDataMgr::getCommodity`、`WTSCommodityInfo`（Includes 内联）、`CodeHelper`（Share 内联）、`WtHelper`，须由主控在 `CoverageTests_btcore` 链接 `WtBtCore`+`WTSTools`；coverage 子目录若非自动 glob 需主控加入本目录 3 个文件。
