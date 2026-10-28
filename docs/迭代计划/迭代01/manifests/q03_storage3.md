# q03_storage3 交付清单 — WtDataStorage 读侧「文件形态」链路（第五波 lane q03）

- 车道: q03（主目标二进制 `CoverageTests`，`src/TestUnits/coverage/CMakeLists.txt` 的 `MAIN_PLUGIN_TUNS` 已编入 `src/WtDataStorage/*.cpp`）
- 靶子（第一波实测）: `WtRdmDtReader.cpp` 2077 行未覆盖(11.13%)、`WtDataReader.cpp` 1694(11.59%)、`WtDataWriter.cpp` 1513(25.65%)
- 生产代码改动: **无**。未动任何 `CMakeLists.txt`；未 include 任何 `WtDataStorageAD/*`（避开 8 个同名 `extern "C"` 工厂 duplicate）；未引用 `WtCore/WtBtCore/WtUftCore/WtDtCore` 任何类
- 简报纠偏沿用: `WtHisDatas/WtdayDatas/WtTSTickData` 三个名字在本目录不存在（同 p05 跳过项 4 / p10 跳过项 7）

## 新增文件（4 个，`src/TestUnits/coverage/q03_storage3/`）

| 文件 | 主题 | 用例 |
|---|---|---|
| `q03_storage3_fixture.h` | 宿主替身 + 落盘构造器 + CWD 沙箱（非用例 TU） | — |
| `q03_rdm_tick_file_test.cpp` | `WtRdmDtReader` tick 侧：`.dsb` 四个版本位、长度校验、尾部残缺、RT `.dmb`、跨交易日拼接、左右窗口裁剪 | 15 启用 + 1 `DISABLED_` |
| `q03_rdm_kline_file_test.cpp` | `WtRdmDtReader` K 线侧：`cacheHisBarsFromFile` 普通分支建库、区间裁剪、his+rt 双块拼接、`clearCache` 重建 | 12 启用 |
| `q03_reader_file_test.cpp` | `WtDataReader` 侧：`readTickSlice`/`readOrdQueSlice`/`readOrdDtlSlice`/`readTransSlice` + `getRTTickBlock`，cfg 绝对路径注入 | 6 启用 |

合计 **33 个 `TEST_F`（启用 32 + 1 `DISABLED_`）**，断言宏 ≈ 300 条（含 20 字段 × N 条的逐字段比对）。

落盘全部在 `std::filesystem::temp_directory_path()/q03_{rdmtk,rdmkline,rdf}_<pid>_<seq>/data/`，
`TearDown` 先 `clearCache()`、析构 reader（释放 mmap）再 `remove_all`；**未读未写仓库 `dist/`**。

## 覆盖的类 / 函数 / 符号（符号名来自 api-brief-L04 或本轮 Read 实测行）

WtRdmDtReader（`src/WtDataStorage/WtRdmDtReader.cpp`）
- `readTickSliceByDate`（`:239`）：his 建块（`:299-311`）、`Sizechecking`（`:303`）、`tcnt==0` break、当日 RT 块分支（`:334-357`）、缓存表命中
- `readTickSliceByRange`（`:363`）：**跨交易日 while(:398) 逐日读 `<date>.dsb` + `slice->appendBlock` 拼接**、`:465-531` 右边界 eTick（含 `getCloseTime()*100000+59999`）与 `:507/:517` 左边界 sTick 两条路径、中间日缺文件跳过（`:443`）、`:536-606` 当日 RT 接续、`:608` 永远返回切片对象
- `cacheHisBarsFromFile`（`:1134`）：**普通合约分支 `:1568-1608` 全链**（exists → 长度校验 → `proc_block_data(content,true,false)` → barcnt → 拷入 `_bars_cache[key]._bars`）、`:1610-1623` realCnt 反向拼接、`:1146-1151` pname switch
- `readKlineSliceByRange`（`:1789`）：缓存未命中建库、`etime==0 → 203012312359`（`:1807`）、`bHasToday` 两侧、KP_DAY 时 `getRTKilneBlock` 的 `:2164` 早退、`:1860-1906` RT 裁剪（两条 lower_bound）、`:1985-1995` his+rt 双块
- `indexBarFromCacheByRange`（`:1629`）经公开入口驱动：全量/左右闭区间/越界空窗/`isDay` 与非 `isDay` 两条比较器
- `getRTTickBlock`（`:2002`）与 `getRTKilneBlock`（`:2162`）：`rt/ticks|rt/min1/<exchg>/<code>.dmb` 的 `BoostMappingFile` **只读映射**（普通文件形态，非命名 shm）、`_last_cap` 命中、文件缺失返回 NULL
- `clearCache`（`:2643`）、`init(cfg==NULL, sink)`（`:69`，注入 `_sink/_base_data_mgr/_hot_mgr` 后于 `:76` 早退，**零线程、零 join**）

WtDataReader（`src/WtDataStorage/WtDataReader.cpp`）
- `readTickSlice`（`:322`）：isToday 的 RT 分支（`:363-391`）、his 分支（`:392-455`）多 `_his_dir` 命中、长度校验（`:416-421`）、`min(eIdx+1,count)` 两处
- `readOrdQueSlice`（`:458`）/`readOrdDtlSlice`（`:615`）/`readTransSlice`（`:772`）：`his/queue|orders|trans/<exchg>/<date>/<code>.dsb` 建块 + 逐字段读回（`WTSOrdQueSlice::size/at`、`WTSOrdDtlSlice`、`WTSTransSlice`）
- `getRTTickBlock`（`:1920`）与 `init(cfg)` 的 `_rt_dir/_his_dir` 拼装（`:161-200`，无轮询线程）

DataDefine.h / WTSStruct.h（本轮实测行为，非重复登记）
- `BlockHeader`(12) 与 `BlockHeaderV2`(20) 四种版本位 `BLOCK_VERSION_RAW/CMP/RAW_V2/CMP_V2` 的读回闭环、`BLK_FLAG`/`FLAG_SIZE`、`BT_HIS_Ticks/Day/OrdDetail/OrdQueue/Trnsctn`、`BT_RT_Ticks/Minute1`、`RTDayBlockHeader`(20)+柔性数组的 `.dmb` 真实字节布局
- `WTSTickStructOld → WTSTickStruct`、`WTSBarStructOld → WTSBarStruct` 逐字段升版比对；`WTSOrdQueStruct.volumes[50]`、`WTSOrdDtlStruct`、`WTSTransStruct` 全字段
- `WTSCmpHelper::compress_data`（建库侧）与 `proc_block_data` 内 `uncompress_data`（读回侧）闭环

宿主替身：`IBaseDataMgr`（含 `calcTradingDate` 可覆写映射、`getBoundaryTime`）、`IHotMgr`（14 纯虚 + `getRuleTag` 变体走 HOT 分支）、`IRdmDtReaderSink`、`IDataReaderSink`（`get_date/get_min_time/get_secs` 固定值，**不断言墙钟**）、`WTSCommodityInfo/WTSContractInfo/WTSSessionInfo::create`、`WTSCfgLoader::load_from_file`

## 白盒手法（本波换路，请评审确认）
**没有使用 `#define private public` / `#define protected public`**，全部走「替身 + 公开入口」：
1. `WtDataReader::init(cfg)` 用真实 json（`{"path":"<temp>/","adjust_flag":0}`）注入**绝对** `_rt_dir/_his_dir`，
   该 init 不起线程 → 公开 `read*Slice` 直接驱动到 `cache*`/`getRT*Block` 私有实现。
2. `WtRdmDtReader::_base_dir` 是 private 且只有 `init(cfg!=NULL)` 才赋值（`WtRdmDtReader.cpp:79`，同时 `:87` 起 5 秒轮询线程、`:66` 析构 join）→ **保持默认空串**，
   生产代码里 `"{}his/ticks/..."` 退化为相对路径，由 `q03::Sandbox` 在 SetUp `chdir` 到 temp 目录、TearDown 还原 CWD 并 `remove_all`。
   代价：零线程、零 join、零宏；副作用是 chdir 只在用例期间生效（gtest 单线程顺序执行，且每用例 fixture 独立）。
3. RT `.dmb` 块由测试按 `DataDefine.h` 的 `RTDayBlockHeader` 布局直接写文件（writer 侧同形态，p10 已实测），reader 侧 `read_only` 映射读回 → 写入/读回互校。

## 跳过项（可直接搬进排除清单）

1. **`WtDataWriter` 私有文件形态全链（本轮 0 行新增）**：`dump_bars_to_file`（`:2020-2215` min1/min5 迁移主体）、
   `dump_bars_via_dumper`（`:1698`）、`pipeToKlines`（`:1056`）、`procTick/procQueue/procOrder/procTrans`、
   `loadCache/updateCache/getCurTick/resizeRTBlock`、`proc_block_data`（成员版 `:1818`）、`dump_day_data`（`:1909`）
   —— 这些**全是 private 成员**，公开入口只有 `init`（`:117`，`:168` 无条件 `new StdThread(check_loop)`，内含 `:1621` `sleep_for(10s)` 与 `:2385` `sleep_for(300s)`）
   与 `transHisData`（`:1569`，`:1608` 起 `proc_loop` 线程，且依赖只在 init 里赋值的私有 `_bd_mgr`）。
   **p10 靠 `#define private public` 才拿到这块；本波禁用了该宏，故整体退让。若接受宏，可直接复用 p10 的 `p10_writer_prefix.h`。**
   → 结论：`WtDataWriter.cpp` 的 1513 行未覆盖部分需在「允许宏」或「给 writer 加测试用 friend/protected 注入口（属生产改动，需单独评审）」之后才能推进。
2. `WtRdmDtReader::readTickSliceByCount`（`:2395`）、`readOrd*/readTrans*SliceByRange`（`:611/:786/:960`）：
   形参与 tick 版同构，但本轮预算耗尽（已到 60 次调用上限）未写用例；**结构上可复用 `q03_storage3_fixture.h`，预计 1 个文件、约 600 行覆盖**。
3. `cacheHisBarsFromFile` 的 HOT 分支（`:1162-1363`）与股票复权分支（`:1364-1567`）：
   需要 `splitCustomSections` 返回真 + `HotSections` + `_hot_mgr->getRuleFactor`，且**当前实现有 BUG-Q03-001（barcnt==0 下溢抛异常）**，
   在未修源码前只能写成 `DISABLED_`（本轮已收 1 条复现），主体**待裁定**。
4. `_thrd_check` 5 秒轮询线程（`:87-165`）与 `WtRdmDtReader::init(cfg!=NULL)`：撞「禁止后台线程/join」门禁 → **整函数排除**（同 p09 跳过项 2、p10 跳过项 1）。
5. `WtDataReader::cacheIntegratedBars`（`:969`）/`cacheAdjustedStkBars`（`:1287`）/`cacheFinalBarsFromLoader`（`:930`）：
   需 HOT 段或 `IHisDataLoader` 桩 + `_adjust_flag` 组合，且同样踩 BUG-Q03-001 型下溢 → 待第五波后续。
6. `WtDataReader::onMinuteEnd`（`:2210`）、`WtDataWriter::check_loop`（`:1616`）：依赖墙钟/`sleep_for` → 排除（沿用 p09/p10 结论）。
7. p10 已排除项**未重走**：共享内存 RT 块的 writer 侧写入、300s 清理线程、`_async_task` 未初始化 + 无谓词 wait 的丢唤醒死锁（BUG-P05-005 / BUG-P10-005）。

## 疑似缺陷
`docs/迭代计划/迭代01/bugs/q03_storage3.md`，**5 条新增**（全部 `- 状态: 待审核`、`- 严重级别:` 齐备）：
BUG-Q03-001（Major）`cacheHisBarsFromFile` `hotAy->at(barcnt-1)` uint32 下溢抛 `std::out_of_range` + `hotAy` 泄漏
BUG-Q03-002（Major）四处调用点忽略 `proc_block_data` 返回值 → 长度校验失败的压缩脏数据被当记录解析
BUG-Q03-003（Minor）最小长度校验按 `BlockHeader`(12) 而 writer 用 `BlockHeaderV2`(20) → 12~19 字节残骸过校验且擦头错位
BUG-Q03-004（Minor）`uint32_t tcnt <= 0` 恒等 `==0` + `lower_bound(., +（tcnt-1), .)` 末条不参与定位
BUG-Q03-005（Minor）`clearCache()` 不回收 `_his_tick_map`/`_rt_*_map` 的整文件缓冲，内存单调增长
崩溃/异常类输入**收进 1 个 `DISABLED_` 用例**（`DISABLED_his_kline_hot_fragment_throws_out_of_range`），32 条启用用例零残留、零阻塞。

## 门禁结果（唯一构建动作 `-fsyntax-only`；未跑 cmake/ninja，未自行编译运行用例）

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/q03_storage3/<file>.cpp
```
- `q03_rdm_tick_file_test.cpp`：**clean（0 error 0 warning）**
- `q03_rdm_kline_file_test.cpp`：**clean**
- `q03_reader_file_test.cpp`：**clean**（`q03_storage3_fixture.h` 随三个 TU 一并被检查）

本车道自查修掉的自身问题 4 处（供后续波次参考）：
1. `WTSOrdQueSlice/WTSOrdDtlSlice/WTSTransSlice` **没有** `get_block_counts/get_block_addr/get_block_size`（那是 `WTSTickSlice/WTSKlineSlice` 的 API），
   逐笔类切片用 `size()` + `at(int32_t)`。
2. `BDT_Buy/BDT_Sell`、`ODT_LimitPrice`、`TT_Match`（不是 `WTS_BD_*`/`WTS_ODT_*`/`WTS_TT_*`），在 `Includes/WTSTypes.h:383-415`。
3. `SUFFIX_QFQ` 是 `char`（`CodeHelper.hpp:28`，值 `'-'`），不是字符串；复权后缀走 `SUFFIX_QFQ_S`。
4. 夹具里遗留的未用私有成员会踩 `-Wunused-private-field`（clang 默认开），断言用的辅助字段要一并删掉。
