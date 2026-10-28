# p12_datafactory_datadef 缺陷登记

- Lane: P12（第四波）
- 范围: `src/WTSTools/WTSDataFactory.cpp`、`src/Includes/WTSDataDef.hpp`
- 状态: 全部「待审核」，未修改任何生产代码

---

## BUG-P12-001 · updateMin1Data(tick) 时段外 tick + 空 K 线时解引用 NULL

- 严重级别: Blocker（潜在空指针解引用）
- 状态: 待审核
- 位置: `src/WTSTools/WTSDataFactory.cpp:202-217`
- 期望: `timeToMinutes` 返回 `INVALID_UINT32` 时安全早退
- 实际: 分支内无条件 `WTSBarStruct *bar = klineData->at(klineData->size()-1);`，
  `klineData` 为空时 `at(-1)` 返回 NULL，随后 `bar->close = ...` 直接解引用
- 可达性: 公开入口 `updateKlineData` 先做了 `isInTradingTime` 校验，因此只能由
  protected 路径（子类/内部调用）传入时段外 tick 触发
- 证据: `src/TestUnits/coverage/p12_datafactory_datadef/p12_factory_update_tick_test.cpp`
  → `DISABLED_CrashClassNotExecuted`（整类崩溃输入收进同一个禁用用例，启用用例零残留）

## BUG-P12-002 · extract/update 秒周期路径对 seconds==0 无校验（整数除零）

- 严重级别: Major（SIGFPE）
- 状态: 待审核
- 位置: `src/WTSTools/WTSDataFactory.cpp:712`（`(curSeconds/seconds)`）、`:1332`
- 期望: `seconds == 0` 时返回 NULL 或参数报错
- 实际: 直接作为除数使用；`extractKlineData(tickSlice, 0, ...)` 与
  `WTSKlineData::setPeriod(KP_Tick, 0)` 后的 tick 更新都会除零
- 证据: `p12_factory_extract_merge_test.cpp` → `DISABLED_OutOfBoundsClass`

## BUG-P12-003 · WTSValueArray / WTSKlineData 的范围守卫与循环边界不一致（越界读）

- 严重级别: Major（容器越界读，UB / ASAN 报错）
- 状态: 待审核
- 位置: `src/Includes/WTSDataDef.hpp:98`（`maxvalue`）、`:131`（`minvalue`）、
  `:522`（`WTSKlineData::maxprice`）、`:548`（`minprice`）
- 期望: 守卫拒绝 `end == size()`
- 实际: `maxvalue/minvalue` 判的是 `end > m_vecData.size()`，随后 `for(i = begin; i <= end; i++)`
  会访问 `m_vecData[size()]`。`WTSKlineData::maxprice` 同形（`end > size()` 放行后 `i <= end`）
- 证据: `p12_factory_extract_merge_test.cpp` → `DISABLED_OutOfBoundsClass`
- 补充: `WTSKlineSlice::maxprice/minprice`（`:334`、`:356`）对空切片/越界不判 NULL，
  `this->at(begin)->high` 直接解引用，属同一类输入，已并入该禁用用例

## BUG-P12-004 · get_block_size 在 K 线切片与 tick 切片上返回值语义不一致

- 严重级别: Minor（接口契约不一致）
- 状态: 待审核
- 位置: `src/Includes/WTSDataDef.hpp:272-278`（越界返回 0）与 `:1460-1466`（越界返回 `INVALID_UINT32`）
- 期望: 同类切片 API 的越界语义一致
- 实际: `WTSKlineSlice::get_block_size(2)` == 0，`WTSTickSlice::get_block_size(2)` == 0xFFFFFFFF；
  调用方无法用统一判据区分「空块」与「非法下标」
- 证据: `p12_factory_extract_merge_test.cpp` → `SliceAssemblyAcrossBlocks`（K 线侧断言 0）

## BUG-P12-005 · extractDayData 合并时 add 用赋值而非累加，与其余周期不一致

- 严重级别: Minor（增仓口径失真）
- 状态: 待审核
- 位置: `src/WTSTools/WTSDataFactory.cpp:1292`（`lastBar->add = curBar.add;`）
  对比 `:1285-1291` 内 high/low/close/vol/money 的累加/取极值写法，
  以及 `updateMin1Data`/`updateMin5Data`/`updateHourData` 的 `+=` 写法
- 期望: 多根合并时增仓应累计
- 实际: 只保留最后一根的增仓值（`hold` 取最后一条是各周期一致的，`add` 不一致）
- 证据: `p12_factory_extract_merge_test.cpp` → `DayResampleGroupsByCount`
  （断言 `add == 5.0`，即第二根的值，锁定当前行为；修复后需同步更新断言）

## BUG-P12-006 · updateDayData/updateSecData 返回裸 new 对象，所有权只靠注释约定

- 严重级别: Minor（易泄漏 / 双重释放）
- 状态: 待审核
- 位置: `src/WTSTools/WTSDataFactory.cpp:679-692`、`:726-739`
- 期望: 返回内部缓冲区指针或由容器接管
- 实际: 新交易日/新秒桶时返回 `new WTSBarStruct` 且不 append 进 `WTSKlineData`，
  调用方必须自行 `delete`；与 `updateMin1Data`（`appendBar` 后返回 `at(-1)`）语义相反，
  同一接口两种所有权
- 证据: `p12_factory_update_tick_test.cpp` → `DayPathReturnsDetachedBar` / `SecPathWithTimesAndUnixTime`
- 备注: 另外 `IDataFactory`（`src/Includes/IDataFactory.h:32`）无虚析构，
  经 `IDataFactory*` 释放实现类是 UB，测试内只做栈对象/显式类型释放

## BUG-P12-007 · WTSValueArray::at/set 对无符号下标做 `idx < 0` 判定（死分支）

- 严重级别: Minor（编译期可判定恒假，负数语义丢失）
- 状态: 待审核
- 位置: `src/Includes/WTSDataDef.hpp:70`、`:165`
- 实际: `at(uint32_t idx)`/`set(uint32_t idx, ...)` 内 `if(idx < 0 || ...)`，
  `idx < 0` 恒假；`at` 依赖 `translateIdx` 支持负数、`set` 不支持，两者能力不对称
- 证据: `p12_factory_extract_merge_test.cpp` → `SliceAssemblyAcrossBlocks`
  （`set(9, 99.0)` 越界静默忽略、`at(99)` 返回 `INVALID_DOUBLE`，负数写入无法表达）

## 参考的既有编号

- BUG-L03-001（`WTSVariant::create` / `WTSHisTickData::create` 默认实参重叠导致的重载歧义）：
  本 lane 未新增 `WTSHisTickData::create` 调用，未再次撞到；若后续需要，
  按该编号记录的写法使用显式 `static_cast` 锚定重载。
