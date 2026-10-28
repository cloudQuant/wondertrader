# L03 交付清单 — src/Includes/ 单元测试

- Lane: L03
- 模块: `src/Includes/`（36 个头文件）
- 交付时间: 迭代01
- 工具调用预算: 30 次内收口（实际约 30 次）

## 1. 已有产物修复（ws03）

| 项 | 内容 |
|---|---|
| 文件 | `src/TestUnits/coverage/ws03_includes_data/ws03_data_def_test.cpp`（893 行） |
| 原错误 | `ws03_data_def_test.cpp:750:26: error: call to 'create' is ambiguous` |
| 根因 | `WTSDataDef.hpp` L1320 `create(const char*, unsigned int nSize = 0, bool = false, double = 1.0)` 与 L1337 `create(const char*, bool bValidOnly = false, double = 1.0)` 的默认实参集合重叠，只传代码时两个重载同样可行 |
| 修复 | 仅改 L750 一行：`WTSHisTickData::create("DCE.i2405", static_cast<bool>(false))`，用显式 `static_cast` 锚定 bool 重载（语义＝不预分配 tick 数组，与原断言一致），并加注释指向 bug 记录 |
| 未做 | 没有重写其余 890 行；未改任何生产代码 |
| 记录 | `docs/迭代计划/迭代01/bugs/l03_includes.md` → BUG-L03-001（状态：待审核） |
| 验证 | 门禁命令重跑，0 error 0 warning |

## 2. 新增文件（`src/TestUnits/coverage/l03_includes/`）

| 文件 | 行数 | 覆盖目标 |
|---|---|---|
| `l03_session_info_test.cpp` | 497 | `WTSSessionInfo` 全量内联实现 |
| `l03_commodity_info_test.cpp` | 259 | `WTSCommodityInfo` |
| `l03_contract_info_test.cpp` | 258 | `WTSContractInfo` |
| `l03_object_pool_test.cpp` | 244 | `WTSObject` / `WTSPoolObject` / `WTSSwitchItem` |

说明：`l03_session_info_test.cpp` 超出 150–400 行建议区间（497 行），原因是单个类的纯函数群（12 个换算函数 × 边界分支）拆成两文件会造成 fixture 与时段构造重复；按主题保持一个文件，未再拆分。

## 3. 覆盖的类/函数

**WTSSessionInfo（`WTSSessionInfo.hpp`）**
`create` / `id` / `name` / `getOffsetMins` / `setOffsetMins` / `addTradingSection` / `getSectionCount` / `getTradingSections` /
`setAuctionTime`（空表与非空表两条分支）/ `addAuctionTime` / `getAuctionSections` / `getAuctionStartTime` /
`getOffsetDate`（当日、次日 `>=1440`、前日 `<0`、跨月跨年闰日、`uDate==0` 当前时间分支）/
`offsetTime`（左右对齐 × `curMinute == 1440` 与 `== 0` 临界、0 点→2400 约定、正负偏移回绕）/
`originalTime`（`m_uOffsetMins == 0` 直通分支 + 回绕）/
`timeToMinutes`（区间内、区间右端、休息段命中/未命中、`autoAdjust` 两分支、竞价短路返回 0、空时段 INVALID）/
`minuteToTime`（正向、`bHeadFirst`、区间右端、总时长、越界兜底、全天候时段 1440→2400）/
`timeToSeconds`（含 `seconds == stopSecs` 的 `-1`、越界、竞价）/
`secondsToTime`（0、4499/4500/4501 边界、总时长、越界 INVALID）/
`getOpenTime` / `getCloseTime`（bOffseted 两侧）/ `getTradingMins` / `getTradingSeconds`（含 `count==0` 全天候兜底）/
`getSecMinList`（缓存复用、空时段 1440 兜底）/ `isInTradingTime`（bStrict 两侧）/ `isLastOfSection`（含 2400↔0 混叠）/
`isFirstOfSection` / `isInAuctionTime`（左闭右开、未配置竞价）

**WTSCommodityInfo（`WTSContractInfo.hpp` L19-L166）**
`create`（含币种默认值与 `m_strFullPid` 拼接、63 字符边界）/ 全部 setter-getter（volScale、priceTick、lotsTick、minLots、marginRate）/
`setCategory` + `isFuture/isStock/isOption/getCategoty`（8 个分类）/ `setTradingMode` + `canShort/isT1` /
`setCoverMode/getCoverMode` / `setPriceMode/getPriceMode` / `addCode` + `getCodes`（去重、find）/
`setSessionInfo/getSessionInfo`（含解绑）/ `setFeeRates` + `calcFee`（算法 0 按量、算法 1 按额、`m_nFeeAlg==-1` 关闭、未知 offset、万分取整进位与舍去）

**WTSContractInfo（`WTSContractInfo.hpp` L168-L357）**
`create` / `getCode/getAltCode/getExchg/getName/getProduct/getFullCode/getFullAltCode/getFullPid` /
`setAltCode`（覆盖写、空串）/ `setVolumeLimits`（默认参数、4 参、0 值）/ `setDates`（默认 19900101/30991231）/
`setMarginRatios` + `getLongMarginRatio/getShortMarginRatio`（flag==1、品种费率非 0、品种费率为 0 回落、默认 flag）/
`setCommInfo/getCommInfo` / `setFeeRates` + `calcFee`（委托品种、按量、按额用品种放大倍数）/
`addHotCode/isFlat/getHotCodes`（不去重）/ `setTotalIndex/getTotalIndex`（0/4096/UINT_MAX）/
`setExtData<INDEX>/getExtData<T,INDEX>`（默认槽位、多槽位、置空）/ `getVolScale/setVolScale`

**WTSObject / WTSPoolObject / WTSSwitchItem**
`WTSObject`：初始计数、`retain` 返回值、`retainCount`、`isSingleRefs`、归零即 `delete this`、`release` 的 `m_uRefs == 0` 早退分支（子类覆写模拟）、在具体类 `WTSSessionInfo` 上的计数生命周期 /
`WTSPoolObject<T>`：`allocate()` 构造计数、池对象引用计数语义、归还后再分配、同池多对象互不串扰 /
`WTSSwitchItem`：`create` 五字段、`_factor` 默认 1.0 与 `set_factor`、空串字段、长字符串（`std::string` 成员）、引用计数生命周期

## 4. 跳过目标及原因

| 目标 | 原因 |
|---|---|
| `WtDefine.h` 内联判定（`isTradeTime` 类） | 该文件在本仓库不存在（`find src -name WtDefine.h` 无结果，`src/Share/` 下只有 BoostFile/TimeUtils/StrUtil/ObjectPool/WtKVCache 等）。任务描述里的候选名对应上游版本，判定函数实际内联在 `WTSSessionInfo.hpp`，已在本轮覆盖。 |
| `WtSPBCache` / `WtCachePool` | `src/**` 下无 `*SPB*` / `*Cache*` 匹配文件；仅有 `src/Share/WtKVCache.hpp`，属 `src/Share/` 模块（不在 L03 范围），且被其他 lane 覆盖。 |
| `WTSVariant.hpp` | 已由 ws03 产物覆盖（含 `create` 重载歧义问题）；L03 不重复建面。 |
| `WTSDataDef.hpp` 的 tick/bar/slice 数据对象 | 已由 `ws03_data_def_test.cpp` 覆盖，重复建设无收益。 |
| `WTSCollection.hpp`（WTSArray/WTSMap/WTSHashMap/WTSQueue/WTSStack） | **未完成**，30 次调用预算耗尽（该文件 940 行需 2 次定向精读 + 1 文件编写 + 1 次门禁）。建议下一轮优先补 `l03_collection_test.cpp`。 |
| `CtaStrategyDefs.h` / `HftStrategyDefs.h` / `UftStrategyDefs.h` / `SelStrategyDefs.h` / `RiskMonDefs.h` / `ExecuteDefs.h` / `I*` 系列接口头 | 纯抽象接口（`virtual ... = 0`）+ 空默认实现，无可执行逻辑；单测需要整套 mock ctx，投产比低，且属于接口契约而非第一方实现。 |
| `IBaseDataMgr.h` / `ISessionMgr.h` / `IHotMgr.h` / `IDataReader.h` / `IDataWriter.h` / `IParserApi.h` / `ITraderApi.h` / `IRdmDtReader.h` / `IBtDtReader.h` / `IExpFactory.h` / `IDataManager.h` / `IIndexDefs.h` / `ILogHandler.h` | 依赖共享内存段（Wtshy/ShmManager）、行情解析器或交易柜台外部服务，或为纯 sink 回调接口；本迭代要求确定性、不起共享内存，跳过。 |
| `WTSExpressData.hpp` / `WTSRiskDef.hpp` / `WTSTradeDef.hpp` | 未排入本轮（预算）；`WTSObject::release/retain` 已在 object_pool 文件中打通，可作为下一轮起点。 |
| `FasterDefs.h` / `WTSStruct.h` / `WTSTypes.h` / `WTSMarcos.h` | 类型/容器别名与 POD 结构定义，无独立可测函数；已在上述测试中被间接构造使用（`wt_strcpy` 的越界风险见 BUG-L03-006）。 |

## 5. 待审核缺陷（`docs/迭代计划/迭代01/bugs/l03_includes.md`）

BUG-L03-001（create 重载歧义，Major）、002（Commodity/Contract 成员未初始化，Blocker）、
003（`isLastOfSection` 2400↔0 混叠，Major）、004（`setOffsetMins` 不重算已登记时段，Major）、
005（`m_commInfo` 空指针解引用，Blocker，已用 `DISABLED_` 用例记录）、006（`wt_strcpy` 无边界检查，Major）、
007（`minuteToTime`/`secondsToTime` 越界语义不一致，Minor）、008（集合竞价被判为交易时间，Minor）、
009（四处不可达回绕/修正分支，Minor，阻塞 `WTSSessionInfo` 100% 行覆盖）、010（零时长时段兜底成 1440，Minor）。
共 **10** 条，全部状态「待审核」，未修改任何生产代码。

## 6. 语法门禁结果

命令（仓库根执行，逐文件）：

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/l03_includes/<file>.cpp
```

| 文件 | 结果 |
|---|---|
| `l03_session_info_test.cpp` | 0 error / 0 warning |
| `l03_commodity_info_test.cpp` | 0 error / 0 warning |
| `l03_contract_info_test.cpp` | 0 error / 0 warning（首轮 4 个错误：`EXPECT_EQ` 宏把模板实参里的逗号当参数切分，已改为局部变量） |
| `l03_object_pool_test.cpp` | 0 error / 0 warning |
| `ws03_data_def_test.cpp` | 0 error / 0 warning（原 1 个歧义错误已修） |

未跑 cmake / ninja / 全量构建（由主控执行）。测试不落任何固定绝对路径、不写临时文件（全部为内存内对象），因此无 `temp_directory_path()` 临时目录需要 TearDown 清理。

## 7. 遗留事项

1. `l03_collection_test.cpp`（`WTSCollection.hpp`：WTSArray / WTSMap / WTSHashMap / WTSQueue / WTSStack）待补。
2. `WTSTradeDef.hpp`（WTSEntrust / WTSOrderInfo / WTSTradeInfo / WTSPositionItem / WTSAccountInfo，786 行、getter 密集）与 `WTSRiskDef.hpp`、`WTSExpressData.hpp` 待补。
3. BUG-L03-005 的 `DISABLED_get_long_margin_ratio_without_comm_info` 需在源码修复评审通过后去掉 `DISABLED_` 前缀。
4. BUG-L03-002 / 009 会让 `WTSContractInfo.hpp` 与 `WTSSessionInfo.hpp` 无法做到 100% 行/分支覆盖，需要评审决定“补默认值 / 删死分支”的修复方向。
