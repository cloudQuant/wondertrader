# P12 交付清单 — WTSDataFactory.cpp + WTSDataDef.hpp（工厂侧）

- Lane: P12（覆盖率迭代01 · 第四波）
- 靶子: `src/WTSTools/WTSDataFactory.cpp`（1450 行，开工前 0.00%）、`src/Includes/WTSDataDef.hpp`（1647 行，开工前 5.08%）
- 落点: `src/TestUnits/coverage/p12_datafactory_datadef/`，目录不在拆分表 → 自动进主目标 `CoverageTests`
- 未改动任何 `CMakeLists.txt`，未改动任何 `src/**` 生产代码

## 1. 新增文件（3 个）

| 文件 | 行数 | 用例 | 覆盖主题 |
|---|---|---|---|
| `p12_factory_update_tick_test.cpp` | 366 | 10（含 1 个 `DISABLED_`） | tick 驱动的 `updateKlineData` 分发 + `updateMin1Data/updateMin5Data/updateDayData/updateSecData`(tick 版) + 引用计数配对 |
| `p12_factory_update_bar_test.cpp` | 354 | 11 | bar 驱动的 `updateMin1Data/updateMin5Data/updateHourData/updateHalfData` + 小节对齐 + 日期哨兵/跨日 + `extractKlineData` 分发校验 |
| `p12_factory_extract_merge_test.cpp` | 494 | 10（含 1 个 `DISABLED_`） | `extractMin1/Min5/Hour/Half/DayData`、`extractKlineData(WTSTickSlice*)`、`mergeKlineData`、`getPrevMinute`、`WTSKlineSlice`/`WTSTickSlice`/`WTSValueArray` 装配与越界拒绝 |

行数说明：前两个文件在 150–400 建议区间内；第三个 494 行略超，
原因是 `extract*` 五个重采样子路径 + `mergeKlineData` + 切片装配同属「工厂产出的装配语义」一条主线，
拆成两文件需要重复 `mkBar/mkTickStruct/fixture` 三件套且会把 `mergeKlineData` 的空/交换/头尾插入用例切断。

合计 31 条用例，28 条启用、3 条 `DISABLED_`（崩溃/越界类输入整类收进同 2 个禁用用例，启用用例零残留）。

## 2. 覆盖的符号（全部来自 api-brief-L02/L03 与本轮实读源码）

`WTSDataFactory`（`IDataFactory` 实现）:
`updateKlineData`(tick 重载 / bar 重载)、`updateMin1Data`(tick / bar)、`updateMin5Data`(tick / bar)、
`updateHourData`、`updateHalfData`、`updateDayData`、`updateSecData`、
`extractKlineData`(WTSKlineSlice* / WTSTickSlice*)、`extractMin1Data`、`extractMin5Data`、
`extractHourData`、`extractHalfData`、`extractDayData`、`mergeKlineData`、`getPrevMinute`。
protected 成员通过测试内派生类 `FactoryProbe` 的 `using` 声明暴露，未改产品头。

`WTSDataDef` 侧本轮新触达的路径（不与 ws03 的 20 条用例重复）:
`WTSKlineSlice::appendBlock`（NULL/count==0 拒绝）与跨 block `at`（正/负/越界）、
`get_block_addr/get_block_size` 越界分支、`extractData` 的 `KFT_SVOLUME/KFT_DATE/KFT_TIME` 分支、
`WTSTickSlice::create(code, NULL, 0)` 空切片、`WTSValueArray::resize/set/at` 的无效值与越界语义、
`WTSKlineData::setPeriod/setUnixTime/isUnixTime/setClosed/appendBar/at`；
`WTSObject`/`WTSPoolObject` 与工厂产出对象的 retain/release 配对。
`WTSSessionInfo`（`create/addTradingSection/timeToMinutes/minuteToTime/timeToSeconds/secondsToTime/getSecMinList/isInTradingTime/isLastOfSection/getCloseTime`）
作为工厂的时间基准被间接驱动，具体断言已由 l03 lane 覆盖，本 lane 不重复建面。

## 3. 跳过目标及原因

| 目标 | 原因 |
|---|---|
| `WTSHisTickData::create` 两个重载 | 默认实参重叠（BUG-L03-001），ws03 已用 `static_cast` 打通一条；本 lane 主攻工厂，不再重复 |
| `WTSTickData`/`WTSTickRef`/`WTSOrdQueData`/`WTSOrdDtlData`/`WTSTransData` 的 getter | 已由 `ws03_data_def_test.cpp` 覆盖，重复无收益 |
| 阻塞/挂死类入口 | 未驱动任何 bind/connect/join、未初始化日志线程池、未启动共享内存；logger 完全未触碰 |
| `updateMin1Data` 时段外 tick 的空指针分支、`seconds==0` 除零、`maxvalue/maxprice` 越界读 | 会导致进程崩溃/UB，整类收进 `DISABLED_CrashClassNotExecuted` 与 `DISABLED_OutOfBoundsClass`，断言已在源码定位 |
| `WTSDataFactory.cpp:755` 之外的 `getPrevMinute` 调用方 | `getPrevMinute` 是 protected static，无内部调用者，已直调覆盖 5 个分支 |

## 4. 落盘与只读约束

所有对象均为内存内构造，用例不落任何文件；`dist/` 未被读写，因此不需要 `temp_directory_path()/p12_*` 临时目录与 TearDown 清理（无残留文件风险）。
用例间不共享状态：fixture 的 SetUp 建 session，TearDown 逐个 `release()`（`WTSKlineData`/`WTSKlineSlice`/`WTSTickSlice`/`WTSValueArray` 归零即释放；`WTSTickData` 归还对象池）；工厂 `updateDayData/updateSecData` 返回的裸 `new WTSBarStruct` 在断言后显式 `delete`。不断言墙钟/时区。

## 5. 待审核缺陷

`docs/迭代计划/迭代01/bugs/p12_datafactory_datadef.md` → BUG-P12-001 ~ BUG-P12-007，共 **7** 条，全部状态「待审核」，未修改源码。

## 6. 语法门禁结果（唯一构建动作，逐文件）

命令（仓库根执行）：

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/p12_datafactory_datadef/<file>.cpp
```

| 文件 | 结果 |
|---|---|
| `p12_factory_update_tick_test.cpp` | 0 error / 0 warning |
| `p12_factory_update_bar_test.cpp` | 0 error / 0 warning |
| `p12_factory_extract_merge_test.cpp` | 0 error / 0 warning |

未跑 cmake / ninja / 测试二进制，未自行编译运行用例（交由主控统一运行判定）。

## 7. 剩余事项（预算超支说明）

工具调用超出 30 次预算约 2 次后收口，以下计划项未落文件，留待下一波：
1. `p12_datadef_slice_test.cpp`：`WTSTickSlice::insertBlock` / `appendBlock` 多块拼接、
   `WTSOrdDtlSlice`/`WTSOrdQueSlice`/`WTSTransSlice` 的 `create(count==0/NULL)` 拒绝与 `at(-1)` 负下标钳制、
   `WTSOrdQueStruct`/`WTSOrdDtlStruct`/`WTSTransStruct` 字段换算与默认值。
2. `p12_datadef_hist_tick_test.cpp`：`WTSHisTickData::appendTick` 复权因子对 price/open/high/low 的
   四字段缩放（不动 volume/turnover）与 `getDataRef` 直接写入的交互。
3. BUG-P12-005（日线 `add` 赋值）修复后需同步更新 `DayResampleGroupsByCount` 的断言。
