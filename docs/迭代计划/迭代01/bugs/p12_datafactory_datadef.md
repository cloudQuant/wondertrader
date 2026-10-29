# p12_datafactory_datadef 缺陷登记

- Lane: P12（第四波）
- 范围: `src/WTSTools/WTSDataFactory.cpp`、`src/Includes/WTSDataDef.hpp`
- 状态: 逐项结论见下文；本轮未修改生产代码或测试

---

## BUG-P12-001 · updateMin1Data(tick) 时段外 tick + 空 K 线时解引用 NULL

- 严重级别: Blocker（潜在空指针解引用）
- 状态: 已撤回（仅能从 protected helper 人工构造；公开入口会先拒绝时段外 tick）
- 位置: `src/WTSTools/WTSDataFactory.cpp:202-217`
- 期望: `timeToMinutes` 返回 `INVALID_UINT32` 时安全早退
- 实际: 分支内无条件 `WTSBarStruct *bar = klineData->at(klineData->size()-1);`，
  `klineData` 为空时 `at(-1)` 返回 NULL，随后 `bar->close = ...` 直接解引用
- 可达性: 公开入口 `updateKlineData` 先做 `isInTradingTime` 校验；`WTSSessionInfo::isInTradingTime`
  对 `timeToMinutes == INVALID_UINT32` 返回 false，因此公开调用不会进入该分支。只可由子类直接调用 protected helper 人工触发。
- 证据: `DISABLED_CrashClassNotExecuted` 仅直接调用 protected helper；它不是公开产品路径的复现，且本轮未运行。

## BUG-P12-002 · extract/update 秒周期路径对 seconds==0 无校验（整数除零）

- 严重级别: Major（SIGFPE）
- 状态: 待验证（seconds==0 可由公开 setter/API 传入；允许输入范围尚无接口合同）
- 位置: `src/WTSTools/WTSDataFactory.cpp:712`（`(curSeconds/seconds)`）、`:1332`
- 期望: `seconds == 0` 时返回 NULL 或参数报错
- 实际: 直接作为除数使用；`extractKlineData(tickSlice, 0, ...)` 与
  `WTSKlineData::setPeriod(KP_Tick, 0)` 后的 tick 更新都会除零
- 证据: 源码除法点静态可见；`DISABLED_OutOfBoundsClass` 未运行，公开接口是否要求正 seconds 尚无合同证据。

## BUG-P12-003 · WTSValueArray / WTSKlineData 的范围守卫与循环边界不一致（越界读）

- 严重级别: Major（容器越界读，UB / ASAN 报错）
- 状态: 静态越界缺陷成立；禁用测试未运行
- 位置: `src/Includes/WTSDataDef.hpp:98`（`maxvalue`）、`:131`（`minvalue`）、
  `:522`（`WTSKlineData::maxprice`）、`:548`（`minprice`）
- 期望: 守卫拒绝 `end == size()`
- 实际: `maxvalue/minvalue` 判的是 `end > m_vecData.size()`，随后 `for(i = begin; i <= end; i++)`
  会访问 `m_vecData[size()]`。`WTSKlineData::maxprice` 同形（`end > size()` 放行后 `i <= end`）
- 证据: `DISABLED_OutOfBoundsClass` 构造了 `end == size()` 输入但未运行；不把该禁用用例当作动态越界证据。
- 补充: `WTSKlineSlice::maxprice/minprice`（`:334`、`:356`）对空切片/越界不判 NULL，
  `this->at(begin)->high` 直接解引用，属同一类输入，已并入该禁用用例

## BUG-P12-004 · get_block_size 在 K 线切片与 tick 切片上返回值语义不一致

- 严重级别: Minor（接口契约不一致）
- 状态: 已撤回（未找到同类 API 必须共用错误 sentinel 的接口合同）
- 位置: `src/Includes/WTSDataDef.hpp:272-278`（越界返回 0）与 `:1460-1466`（越界返回 `INVALID_UINT32`）
- 期望: 同类切片 API 的越界语义一致
- 实际: `WTSKlineSlice::get_block_size(2)` == 0，`WTSTickSlice::get_block_size(2)` == 0xFFFFFFFF；
  两种接口返回值不同，但调用方是否需要统一判据没有合同证据
- 证据: `SliceAssemblyAcrossBlocks` 只断言 K 线越界返回 0；未证明调用方对两种 sentinel 有统一处理要求。

## BUG-P12-005 · extractDayData 合并时 add 用赋值而非累加，与其余周期不一致

- 严重级别: Minor（增仓口径失真）
- 状态: 待验证（赋值与其他聚合路径不同；`add` 的跨日重采样业务口径待确认）
- 位置: `src/WTSTools/WTSDataFactory.cpp:1292`（`lastBar->add = curBar.add;`）
  对比 `:1285-1291` 内 high/low/close/vol/money 的累加/取极值写法，
  以及 `updateMin1Data`/`updateMin5Data`/`updateHourData` 的 `+=` 写法
- 期望: 多根合并时增仓应累计
- 实际: 只保留最后一根的增仓值（`hold` 取最后一条是各周期一致的，`add` 不一致）
- 证据: `DayResampleGroupsByCount` 本机运行 PASS，但断言 `add == 5.0` 只是锁定当前源码行为，不能作为语义 oracle。`updateDayData` 和分钟/小时聚合路径累加 additional，支持“应累计”的推测；仍需产品口径或独立样例确认。

## BUG-P12-006 · updateDayData/updateSecData 返回裸 new 对象，所有权只靠注释约定

- 严重级别: Minor（易泄漏 / 双重释放）
- 状态: 静态产品路径缺陷成立（`WtDataManager` 忽略返回值）；测试 helper 所有权断言另见复核记录
- 位置: `src/WTSTools/WTSDataFactory.cpp:679-692`、`:726-739`；调用方 `src/WtDtServo/WtDataManager.cpp:567-606`
- 期望: 对外接口的返回值所有权与容器更新语义应明确且一致；调用方必须接管 detached bar 或工厂 append 后返回容器项
- 实际: 新交易日/新秒桶时返回 `new WTSBarStruct` 且不 append。`WtDataManager::update_bars` 忽略 `updateKlineData()` 返回值，随后读取 `kData->at(-1)` 并触发 bar 回调；因此新日 bar 被丢弃，日线回调仍读旧 bar，detached 对象也无人释放。`updateMin1Data` 则 append 后返回容器末项，语义不同。
- 证据: 两个测试直接调用 helper，并不覆盖 `WtDataManager::update_bars` 忽略返回值的产品调用链；动态产品回调尚未测。
- 备注: 另外 `IDataFactory`（`src/Includes/IDataFactory.h:32`）无虚析构，
  经 `IDataFactory*` 释放实现类是 UB，测试内只做栈对象/显式类型释放

## BUG-P12-007 · WTSValueArray::at/set 对无符号下标做 `idx < 0` 判定（死分支）

- 严重级别: Minor（编译期可判定恒假，负数语义丢失）
- 状态: 已撤回（uint32_t 参数未承诺支持负索引；现有断言未证明负数写入是有效接口行为）
- 位置: `src/Includes/WTSDataDef.hpp:70`、`:165`
- 实际: `at(uint32_t idx)`/`set(uint32_t idx, ...)` 内 `if(idx < 0 || ...)`，
  `idx < 0` 恒假；`at` 依赖 `translateIdx` 支持负数、`set` 不支持，两者能力不对称
- 证据: `p12_factory_extract_merge_test.cpp` → `SliceAssemblyAcrossBlocks`
  （`set(9, 99.0)` 越界静默忽略、`at(99)` 返回 `INVALID_DOUBLE`，负数写入无法表达）

## 参考的既有编号

- BUG-L03-001（`WTSVariant::create` / `WTSHisTickData::create` 默认实参重叠导致的重载歧义）：
  本 lane 未新增 `WTSHisTickData::create` 调用，未再次撞到；若后续需要，
  按该编号记录的写法使用显式 `static_cast` 锚定重载。

## 2026-10-29 独立复核与本机测试记录

本节仅复核源码、接口和现有测试定义；没有改生产代码或测试。覆盖结果来自现有 `build-cov` 二进制，不代表其它平台或发布构建。

### 逐项分类与处理

- **BUG-P12-001 — 已撤回。** `updateMin1Data(tick)` 的 protected helper 对空 K 线和非法时间存在空指针风险，但公开 `updateKlineData(tick)` 先调用 `sInfo->isInTradingTime(...)`；该方法在 `timeToMinutes()` 返回 `INVALID_UINT32` 时返回 false，所以本条给出的时段外 tick 不会由公开入口传到该分支。修复若要保留，应先定义是否允许子类绕过公开入口调用 helper；当前不按现行产品路径登记。
- **BUG-P12-002 — 待验证。** `seconds==0` 的除法点静态成立，且 `WTSKlineData::setPeriod` 不校验周期倍数；但 API 未写明零是否为有效输入。本轮未运行除零输入。修复方向是在 setter 和 extract/update 边界拒绝零值，或在接口文档明确正数前置条件并校验。
- **BUG-P12-003 — 静态越界缺陷成立。** `maxvalue/minvalue/maxprice/minprice` 对 `end==size()` 放行后按 `i<=end` 访问；切片版本还可能在空/无效范围上解引用 `at(begin)` 的空指针。`DISABLED_OutOfBoundsClass` 定义了越界输入但未运行，因此结论限于源码控制流，不声称有 ASAN/运行时报告。修复方向：统一半开区间或把包含式末端校验改为 `< size`，并在空切片/无效范围时先返回 sentinel。
- **BUG-P12-004 — 已撤回。** K 线和 tick 切片越界返回不同 sentinel 是源码事实，但没有发现接口承诺两类 API 必须共用 sentinel，现有测试也没展示调用方统一处理失败。因此目前只是风格差异，不按功能缺陷登记。
- **BUG-P12-005 — 待验证。** `extractDayData` 对 `add` 赋值而非累加为静态差异；更新日线、分钟、小时聚合会累计 additional，支持累计口径推测，但 `DayResampleGroupsByCount` 的断言只锁定当前行为，不是业务 oracle。待确认 `add` 的跨日重采样定义后再改实现/断言。
- **BUG-P12-006 — 静态产品路径缺陷成立。** `updateDayData` 在新交易日返回 detached bar；`WtDataManager::update_bars` 忽略工厂返回值后读取原 `kData->at(-1)` 并触发回调，新日数据因此未进入缓存且返回对象未释放。修复方向是让工厂 append 并返回缓存项，或由调用方接管返回对象且用它更新缓存/回调；需补产品调用链测试。现有两个测试直接调用 helper，未验证真实回调路径。`IDataFactory` 无虚析构的观察单列为接口风险；本轮未发现通过该基类 delete 的调用，不并入本缺陷。
- **BUG-P12-007 — 已撤回。** `idx < 0` 对 `uint32_t` 是死分支的源码事实，但接口参数并未承诺支持负索引；现有测试只验证普通越界静默返回，不能把负索引能力不对称判为调用方可观察故障。

### coverage 运行结果与失败归因

- 目标二进制：`build-cov/build_x64/Debug/bin/TestUnits/CoverageTests`；修正前 `P12*.*` 为 29 项、22 PASS、7 FAIL、2 DISABLED。修正后完整过滤 `P12*.*` 为 29/29 PASS、2 DISABLED；禁用项未运行，也不代表 BUG-P12-006 的产品调用链已验证。
- `P12TickFactory.DayPathReturnsDetachedBar`：旧 fixture 在手工 seed 时漏赋首 tick 的 `add=12`，现已补齐；保留累加后的 14 断言，重建后单例 PASS。
- `P12TickFactory.SecPathWithTimesAndUnixTime`：旧测试删除第一次 detached bar 却未 append 到 `m_kline`，现已先复制追加再释放 detached 对象；重建后单例 PASS。真实 `WtDataManager` 消费行为仍由 BUG-P12-006 记录。
- `P12BarFactory.HourResamplePaths` / `HalfResamplePaths`：旧测试混淆交易日和自然日，现使用合法夜盘并分别校验交易日期及跨午夜的自然时间，重建后 PASS。
- 原 `P12BarFactory.Minute1DateRollWhenTargetTimeFallsBehind`：23:30 不在 09:00–10:00 会话内，旧结果依赖无效分钟值的无符号运算。已改用合法收盘时间并重命名；重建后 PASS，该用例不证明非法时段的产品行为。
- `P12Extract.Minute5ResampleAndAligned` / `P12BarFactory.Minute5Times1And2`：旧夹具将非五分钟时间戳或一分钟序列送入五分钟聚合。现改为 09:05/09:10/09:15 五分钟基础线、修正输入 helper 的 close 值，并检查桶时间、量及收盘价；重建后 PASS。
- `P12Extract.DayResampleGroupsByCount` PASS，但 `add==5` 只说明测试与当前赋值行为一致，不能证明跨日 add 语义。
