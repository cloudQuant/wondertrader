# lane P11 manifest — WtCore/TraderAdapter 交易回报链与风控实体分支

工作流: 测试覆盖率迭代01 / 第四波 lane P11。
靶子: `src/WtCore/TraderAdapter.cpp`（2423 行，开工前实测 1751 行未覆盖、5.91%）。
链接目标: `CoverageTests_wtcore`（只编译 `src/WtCore/*.cpp` + WTSTools/WTSUtils 静态库）。
**未修改任何 `src/**` 生产代码与 `CMakeLists.txt`**（`src/TestUnits/coverage/CMakeLists.txt` 用
`GLOB_RECURSE *.cpp`，新目录自动纳入；新增的 `.h` 不是 `.cpp`，不会被 GLOB 收进源文件列表）。

## 新增文件（行数/用例数按磁盘统计）

| 文件 | 行数 | TEST_F | 其中 DISABLED_ |
|---|---|---|---|
| `src/TestUnits/coverage/p11_wtcore_traderadapter/p11_trader_fixture.h`（公用替身，非 .cpp） | 326 | — | — |
| `.../p11_sync_report_test.cpp` | 369 | 4 | 0 |
| `.../p11_push_notify_test.cpp` | 413 | 7 | 0 |
| `.../p11_preorder_risk_test.cpp` | 373 | 8 | 0 |
| `.../p11_push_persist_test.cpp` | 291 | 5 | 0 |
| `.../p11_crash_class_test.cpp` | 144 | 3 | 1 |
| 合计（5 个 .cpp） | 1590 | 27 | 1（启用 26 条） |

`p11_push_notify_test.cpp` 略超 400 行上限（回报链要覆盖的分支多，未再拆文件）；其余四个都在 150–400 区间。

## 替身与驱动方式

- `p11fix::FakeTraderApi`: 自造的 `ITraderApi` 替身。`ITraderApi` 全部方法都带默认实现，
  所以只需覆心想要的：`init/registerSpi/connect/makeEntrustID/orderInsert/orderAction/query*`，
  记录调用次数与最后一次委托快照（改写后的 code/exchg/entrustID/usertag/dir/offset/pxtype/flag）。
  不起线程、不联网、不接真实柜台。`run()` 只做 `registerSpi + connect`，非阻塞。
- `p11fix::RecordingSink`: `ITrdNotifySink` 订阅者，记录 `on_trade/on_order/on_position/on_entrust/on_account/on_channel_*`。
- `p11fix::FakeBaseDataMgr`: `IBaseDataMgr` 替身（沿用 n01 同名替身的接口实现形状），
  登记 `SHFE.ag.2512(ag2512)` 与 `SHFE.au.2212(au2212)` 两张合约表。
- 编译入口先显式 `#include <boost/core/noncopyable.hpp>` 与 `#include <WtCore/ITrdNotifySink.h>`
  再包含 `WtCore/TraderAdapter.h`：该头只在 :28 前向声明 `ITrdNotifySink`（`RecordingSink` 要继承它，
  不单独包含就是 `base class has incomplete type`，首轮门禁实测），并在 :567 用 `boost::noncopyable`
  却没 include 它（即 **BUG-L00-004**），本 lane 不为了测试去改产品头。

## 覆盖的函数/分支（符号级）

### p11_sync_report_test.cpp（同步查询回报链）
- `onLoginResult` 成功(:1426-1443)/失败(:1428-1435) 两条分支；失败不触发查询，成功后 `_trading_day` 落地 + `queryPositions`。
- `onRspPosition`(:1545-1602): 空数组仍推进状态机(:1596-1601)、分月代码标准化(:1560-1563)、`cInfo==NULL` continue(:1553)、
  多头/空头持仓表覆盖写入（非累加）、`on_position` 每持仓各发多空两条(:1586-1590)、`printPosition`(:1332-1337)。
- `onRspOrders`(:1604-1730): `_orders` 首建(:1608)、`_undone_qty` 全量重算(:1611/:1710-1711)、
  `orderInfo==NULL` continue(:1616)、`cInfo==NULL` continue(:1620)、`setCode(cInfo->getCode())` 回写原始代码(:1623)、
  买/卖判定(:1625)、`_orderids` 登记(:1636)、`_stat_map` 懒建(:1638-1643)、
  买开/卖平统计(:1645-1693)、错单 `isError` 与 `WOS_Canceled` 分支(:1650-1667/:1675-1692)、
  死单不入表(:1695)、外部单 usertag 前缀不匹配不入表(:1698)、同 localid 替换释放旧对象(:1707)、状态推进(:1724-1729)。
- `getOrders`(:467-485): `_orders==NULL` 返回 NULL、`isAll`(:472)、按原始代码过滤(:481)、`OrderMap::create/add/get/size`。
- `onRspTrades`(:1739-1816): 按 `CC_Future` 标准化(:1755-1756)、NULL cInfo continue(:1748)、
  多头开/平/平今统计(:1777-1785)、空头开/平统计(:1786-1794)、`checkSelfMatch`(:1818-1853) 的
  首次登记/同号不同关联单判自成交/同号同关联单放过三条分支、`tInfo==NULL` 返回 false(:1820)、状态推进。
- `onRspAccount`(:1514-1543) + `saveData`(:303-390) + `queryFund`(:2356-2362) + `handleEvent(WTE_Close)`(:1418-1423) + `release`(:419-426)。
- `getPosition`(:428-452) flag 1/2/3 × bValidOnly 全组合、未登记合约返回 0、`enumPosition`(:454-465)。

### p11_push_notify_test.cpp（实时推送回报链）
- `onPushOrder`(:1873-2146): 空指针(:1875)、无 cInfo(:1879)、分月/现货期权标准化(:1886-1891)、
  `isNet()/isBuyAction()` 买卖判定(:1893)、撤销统计四分支(:1896-1949: 买/卖 × 错单/普通撤单/自动撤单)、
  首笔订单号计数(:1955-1979)、平仓预占可平量「先昨后今 + 下限截断」(:1982-2039)、平今只动今仓(:1994-1997)、
  重复推送不重复预占(:1957-1958)、撤销回补可平量且昨仓溢出转今仓(:2042-2084)、
  迟到推送（已知订单号 + 全部成交）两分支都不进(:2042-2084)、
  外部单 localid=0 不分发不入表(:2092-2136)、死单在 `_orders==NULL` 时被 else 分支塞进订单表(:2120-2130)、
  未完成量只在撤销回报时回补(:2099-2112)、`logOrder`(:2139-2142)。
- `onPushTrade`(:2148-2335): 非净头寸的开/平/平今对多空两侧持仓与可用的改写(:2184-2245)、
  T1 判定(:2196/:2223，替身用 `TM_Both` 走非T1)、净头寸「先平昨再平今再开反向」(:2246-2306)、
  未完成量按买卖方向累加(:2310-2319)、多订阅者 `on_trade`(:2321-2323)、自成交拉黑(:2329)、
  成交回报后固定 `queryAccount`(:2334)。
- `onRspEntrust`(:1450-1512): `err==NULL`/`WEC_NONE` 短路(:1452)、错单标准化(:1458-1463)、
  动作名拼装(:1470-1477)、**未完成量为 0 时视为重复通知直接返回**(:1487-1489)、
  买/卖方向回落(:1491-1492)、有 usertag 时 `on_entrust` 分发(:1494-1505)、外部委托只回落不通知(:1506-1510)。
- `checkSelfMatch` 经推送链再次覆盖（重复推送放过 / 同号不同关联单拉黑 / 他品种不受影响）。

### p11_preorder_risk_test.cpp（下单前置改写 + 风控实体分支）
- `doEntrust`(:487-520): `makeEntrustID`(:489)、cInfo 回查(:491-492)、
  `setCode(getAltCode())/setExchange()`(:498-500)、`makeLocalOrderID` + usertag 打标记(:502-506)、
  提交失败返回 `UINT_MAX`(:508-513)、成功后写 `_order_time_cache`(:516-518)。
- `openLong/openShort/closeLong/closeShort`(:1331-1401) 的限价/市价选择与开平标志组合，
  断言 `usertag == "otp.p11r.<localid>"`、`api_.last_code_ == "ag2512"`、`last_entrust_id_` 来自替身。
- `doCancel`(:1244-1269) + `cancel(localid)`(:1271-1291) + `cancel(stdCode,isBuy,qty)`(:1293-1329):
  死单直接 false(:1246)、按代码撤单的买/卖方向筛选与 `isAll`(:1306-1323)、
  通道拒发（`orderAction<0`）返回 false(:1265-1268)、撤销回报到达后订单被摘除再撤为 false、
  风控排除队列生效后撤单不下发(:1259-1260)。
- `init`(:99-207) 的 riskmon 解析段: `active=false` 分支(:153-156)、缺 riskmon 段分支(:158-161)、
  `policy` 逐条装载 + 缺 default 的 WARN(:147-151)、`module` 缺失时 `return false`(:163-164)
  却已把 `_risk_mon_enabled/_risk_params_map` 落地（实测可据此驱动风控实体分支）。
- `checkCancelLimits`(:544-596) 实体分支: 排除队列短路(:549-550)、`getRiskParams`(:392-402)
  品种命中 `SHFE.au` 与回落 `default`、`total_cancels >= cancel_total_limits` 封禁进 `_exclude_codes`(:557-563)。
- `checkOrderLimits`(:609-661) 同一族分支（排除队列命中）。
- **实测结论**: 时间缓存按「交易所原始代码」写入(:517/:1286) 而检查按「标准代码」读(:566/:631)，
  频率窗口分支不可达（用例 `order_time_cache_never_hits_std_code` 把这一现状钉死，见 BUG-P11-003）。

### p11_push_persist_test.cpp（落盘链，只有回报真到了才走到）
- `initExt`(:209-235) → `initSaveData`(:237-278): 新文件写 `trades.csv`/`orders.csv` 表头、
  已存在文件走 `seek_to_end`(:256-259/:271-274) 且表头不重复。
- `logTrade`(:280-289) + `formatAction`(:56-76) 的 OL/OS/CL/CS 四种标记，字段顺序与值逐字节断言。
- `logOrder`(:291-301): 只有订单结束才追加、`canceled` 标记取 `TRUE`、`remark` 取柜台 `stateMsg`。
- `saveData`(:303-390): `positions` 数组（long/short 各 4 字段）、`funds` 对象（按币种建键，11 个字段）、
  `rtdata.json` 落盘；空资金数组只落盘不分发。

### p11_crash_class_test.cpp（启用 2 条 + 收口 1 条）
- 启用: `onTraderError`（有错误码 / `err==NULL`，`_notifier==NULL` 时安全）、`handleTraderLog`(:2351-2354)、
  `onLogout`(:1445)、`handleEvent(WTE_Connect, ec!=0)`(:1413-1416)、`queryFund`(:2356-2359)、
  `release` 之后状态不变；`TraderAdapterMgr` 双通道 `addAdapter/run/refresh_funds/getAdapter/release`(:2369-2422)
  的**通道间隔离**（登录回报只驱动自己那条 api）。
- `DISABLED_report_paths_that_dereference_null_before_run`: 崩溃类收口（7 个入口，见 BUG-P11-001/002/004/005）。

## 跳过清单（供评审排除）

1. `buy()`(:663-954) / `sell()`(:956-1242) 整段实体路径:
   入口就是 `sInfo->isInTradingTime(WtHelper::getTime(), true)`(:680/:973) ——
   既依赖进程墙钟（断言结果不可复现），又要求 `WTSCommodityInfo` 挂上真实 `WTSSessionInfo`
   （替身给 NULL 会直接段错误），还要 `ActionPolicyMgr` 装载动作规则组 + `_stat_map` 开平限额；
   构造这一整套需要基础数据配置文件，属「需要完整引擎」类（与 n01 的 C 类跳过同因）。
   → 已列入 BUG-P11-002/007 的实测依赖说明，启用用例零触碰，DISABLED 用例仅登记。
2. `init()` 的 module 装载段(:166-206): 交易插件是 `SHARED + hidden 可见性`，只能 dlopen 得到（同 BUG-L00-002）；
   本 lane 只测到 `module` 缺失/为空的早退分支(:163-164) 与其之前的 riskmon 解析段。
3. `isTradeEnabled`(:598-607): 头文件声明 inline、`.cpp` 定义未带 inline，
   单独包含 `WtCore/TraderAdapter.h` 调用会触发 `-Wundefined-inline`（即 n01 的 BUG-N01-008，本 lane 复核仍在），
   故启用用例改断言 `checkCancelLimits/checkOrderLimits` 的等价分支。
4. `init_outputs/EventNotifier` 相关分发: `_notifier` 一律传 NULL（`EventNotifier` 依赖 libwtpy + MQ server，同 n01 的 B 类跳过），
   所以 `onLoginResult/onRspEntrust/onTraderError` 里的 `_notifier->notify(...)` 分支只登记不启用。
5. 跨核心同名类: 本 lane 只用 `Includes/`、`Share/` 接口与内联，未引用 `WtUftCore/WtBtCore/WtDtCore` 的任何类，
   未改 `CMakeLists.txt`。

## 四条实测教训的执行情况

1. 崩溃/挂死类输入整类收进同一个 `DISABLED_` 用例（`p11_crash_class_test.cpp` 唯一一条），启用用例零残留。
2. 无阻塞路径: 未调用 `run()` 之外的任何等待；`TraderAdapter::run()` 与 `TraderAdapterMgr::run()` 都只做到
   `registerSpi + connect`（替身为空操作），不含线程/条件变量；墙钟类输入（`buy/sell`）一律不进启用用例。
3. 显式 include: `boost/core/noncopyable.hpp`、`WtCore/ITrdNotifySink.h`、`Includes/IBaseDataMgr.h`、
   `Includes/WTSContractInfo.hpp`、`Includes/WTSTradeDef.hpp`、`Includes/WTSRiskDef.hpp`、
   `Includes/WTSCollection.hpp`、`Includes/WTSVariant.hpp`、`WTSUtils/WTSCfgLoader.h`、`WtCore/WtHelper.h`。
   `TraderAdapter.h` 只前向声明 `ITrdNotifySink`，因此 `RecordingSink` 所在夹具必须自己包含它（重复包含无害）。
4. 符号名全部来自 api-brief-L07 行号、同轮 Read 结果与编译报错清单，未凭记忆造名。

## 落盘与状态纪律

- 配置文件（risk.json/freq.json/inactive.json/bare.json/second.json/crash.json）只写
  `std::filesystem::temp_directory_path()/p11_<test>_<pid>/`，`TearDown` 里 `remove_all`。
- `initExt` 强制 `_save_data=true`，`initSaveData` 会把 `trades.csv/orders.csv/rtdata.json`
  写到被测代码自己的根目录 `WtHelper::getBaseDir()/traders/<id>/`（该值进程级固化，测试侧无法改道），
  夹具按同一规则拼路径并在 `TearDown` 删除；`p11_push_persist_test.cpp` 的 `SetUp` 还先删一次该目录，
  保证「新文件写表头」分支与历史运行无关。`dist/` 全程只读。
- 用例之间零共享状态: 每个 TEST 自建 `TraderAdapter`（栈对象）与替身；`id_` 按用例取 `p11a/p11r/p11p`，
  订单标记前缀 `otp.<id>.` 决定 localid 解析，断言里 localid 一律来自 usertag 文本，与墙钟无关。
- 未断言任何墙钟绝对值/时区；`makeLocalOrderID()` 只断言「非 0、与 usertag 尾号一致、两次不同」，
  不断言其绝对值（n01 已断言过单调性，不重复）。

## 门禁结果

规定的 `clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror ...` 逐文件执行：
**5/5 个 .cpp 全部 0 error 0 warning**。未跑 cmake/ninja/全量构建/测试二进制，未自行编译运行用例。
首轮门禁暴露并已修掉 4 类问题：
`ITrdNotifySink` 基类不完整（需显式包含 `WtCore/ITrdNotifySink.h`）、`boost::noncopyable` 缺失、
`wtp::WtHelper`/`wtp::WTSCfgLoader` 命名空间误挂（两者都在全局/NS_WTP 之外）、
gtest 里 `NULL` 与指针比较触发 `-Werror`（改 `nullptr`）。

## 待主控处理

1. 疑似缺陷 7 条（Blocker 2 / Major 2 / Minor 3），状态一律「待审核」:
   `docs/迭代计划/迭代01/bugs/p11_wtcore_traderadapter.md`（BUG-P11-001 ~ 007），未动源码。
2. `src/TestUnits/coverage/p11_wtcore_traderadapter/` 新增 1 个 `.h` + 5 个 `.cpp`；
   `.h` 不在 `GLOB_RECURSE *.cpp` 的收集范围里，只被同目录 `.cpp` 相对包含，无需改构建脚本。
3. 本 lane 未链接 `WtUftCore/WtBtCore/WtDtCore` 任何符号，37 个同名 `wtp::` 类的静默归因风险不涉及。
