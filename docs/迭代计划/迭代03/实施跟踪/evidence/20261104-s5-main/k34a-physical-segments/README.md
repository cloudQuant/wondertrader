# 决策 K34=A：AD 读端 HOT/custom 规则代码按物理分段拼接（BUG-R122-002 第二步）

主线程，2026-11-05。裁定“先 C 再 A”，C 已于 `../k34c-ad-rule/` 落地。HEAD 见 `HEAD.txt`，改前（K34=C）读端在 `before/`，改前用例在 `before-tests/`，改后副本在 `after/`，哈希见 `sources.sha256`。

## 改动（`src/WtDataStorageAD/WtDataReaderAD.{h,cpp}`）
- **分段**：规则代码由 `IHotMgr::splitCustomSections(tag, 品种, 19900102, endTDate)` 得到分段（HOT/2ND 也走同一接口）。没有分段时拒绝并记错误。
- **段内读取**（`read_section_bars`）：每段只读该段的物理合约。
  - LMDB 键范围：分钟线取段起始交易日开盘边界前 60 分钟（保留集合竞价）到段末交易日收盘边界，并截到请求 cutoff；日线直接用段起止日期。
  - 读到的记录再按自身交易日（`WTSBarStruct.date`、`WTSTickStruct.trading_date`）筛到段内。换约后旧合约继续交易的数据、新合约成为主力前的数据都不取。
  - 边界由 DateMapping 换算。换算失败或结果不是可用的分钟时间时，整次读取拒绝：缺边界时只能扫到段外最新数据，因此不降级。
- **复权**：`HOT-` 前复权以最新段因子为基准，各段乘 `段因子/基准`；`HOT+` 后复权各段乘段因子；不复权不换算。只换算 OHLC，与文件读端相同（AD 读端无 adjust_flag 配置）。
- **K 线缓存**（`cacheRuleBars`）：
  - 全部分段读取并校验成功后才发布；缓存记录规则标签、品种、最新段物理合约、起始交易日和因子。
  - `readKlineSlice` 的增量更新与 RT 缓存都改读最新段物理合约：只接受起始交易日之后的 K 线，并按因子换算（`update_cache_from_lmdb` 新增 factor 与 minTradingDate 参数，非规则缓存取默认值，行为不变）。
  - 规则映射到的物理合约变化时，整体重建。
- **onMinuteEnd**：规则缓存跟随最新段物理合约更新；规则已切换时先重建，再对新增 K 线逐根回调 `on_bar`。回调使用副本，回调内重入不会破坏遍历。
- **tick**（`readRuleTickSlice`）：同样按分段与交易日拼接，不缓存，切片持有自有缓冲区。
- 预合并的品种键历史不再使用，也不回退到“当前映射原始合约从头扫描”。

## 用例
- 新增 `p05_datastorage2/s5_main_k34a_physical_segments_test.cpp`，按 YYYYMMDDHHMM 返回边界的基础数据替身加按切换表计算分段的 hot mgr：
  1. `HotBarsAreStitchedFromPhysicalSectionsOnly`：旧合约段内 2 根 + 新合约段内 2 根；排除旧合约换约后、新合约成为主力前的数据以及预合并品种键；count=3 取最新 3 根。
  2. `ForwardAdjustedHotScalesOlderSectionsByTheirFactor`：旧段乘 0.8/1.0。
  3. `HotTicksAreStitchedByTradingDate`
  4. `CacheFollowsTheRuleSwitchAtMinuteEnd`：切换前只有旧合约；onMinuteEnd 切换后重建，回调 2 次（最后一根 3901），之后读到 4 根。
  5. `UnusableSectionBoundariesFailClosed`
- **按裁定改写** `R100ADReaderTest.R100ReaderADHotReloadDayAndRtCacheGrowth`：仍为 tick 与 K 线均拒绝、各记 1 条错误、预合并品种键数据不被使用。只把错误文本断言从 K34=C 的 “refuses rule-mapped code” 改为 “rule sections of EXF.rb.HOT”（本夹具 hot mgr 不提供分段）。改前副本在 `before-tests/`。

**红控**：改前（K34=C）读端重建，6 例全部失败（`logs/red-*.log`）：
- 新增 4 例返回 NULL；
- 边界用例与 R100 用例的错误文本不符。

**绿控**：恢复改后文件（cmp 一致）并全量重建后 6/6 通过（`logs/green-*.log`）。

**逐例回归**：adstorage/dtservo/porter/wtcore/m_WtRunner/m_QuoteFactory/dtcore 共 2044 例，2034 通过、10 跳过（`regression-summary.json`）。

## 未覆盖与限制
- `WtRdmDtReaderAD`、`WtBtDtReaderAD` 不处理规则代码，只接受物理代码，未改。非 AD 读端（WtDataReader/WtRdmDtReader）的 HOT 来源维持现状（预合并历史加物理增量）。
- 规则 tick 每次读取都按分段重读，不缓存，高频读取有性能代价，未做性能测试。
- 分段内某物理合约缺数据时，拼接结果在该段留空，不报错。
- 规则切换重建只在 `onMinuteEnd` 带 endTDate 时检测；不带交易日的调用要等下次 `readKlineSlice` 检测。
- 真实 hots.json/custom 规则与真实 AD 数据未验（X）。
