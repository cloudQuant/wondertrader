# q03_storage3 疑似缺陷清单（第五波 lane q03，WtDataStorage 读侧）

- 车道: q03（`src/TestUnits/coverage/q03_storage3/`）
- 生产代码改动: 无。以下只记录，不修源码。
- 与既有清单的关系: BUG-P09-001/002/005、BUG-P10-004、BUG-L04-002/003 同源项**引用原编号**，不重复登记。

---

## BUG-Q03-001
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDataStorage/WtRdmDtReader.cpp:1189/:1194/:1196`（`cacheHisBarsFromFile` 主力连续分支），同型见 `:1431/:1435/:1437`（股票复权分支）
- 描述: `proc_block_data(content, true, false)` 之后 `uint32_t barcnt = content.size()/sizeof(WTSBarStruct)`，
  紧接着无条件 `hotAy->at(barcnt - 1)`。前面的长度校验只保证 `content.size() >= sizeof(HisKlineBlock)`（=12 字节），
  剥掉 12 字节块头后载荷完全可能不足 88 字节 → `barcnt == 0` → `barcnt - 1` 是 uint32 下溢（4294967295）→
  `std::vector::at()` 抛 `std::out_of_range` 逃出库函数；同时 `hotAy`（`:1189` 处 `new`）泄漏。
- 触发输入: `his/day/<exchg>/<exchg>.<product>_<ruletag>.dsb` 大小落在 [12, 99] 且版本位为 `BLOCK_VERSION_RAW_V2`。
- 复现: `q03_rdm_tick_file_test.cpp` 的 `DISABLED_his_kline_hot_fragment_throws_out_of_range`（`--gtest_also_run_disabled_tests` 可跑）。
- 期望: `barcnt == 0` 时视为空文件跳过，并释放 `hotAy`。

## BUG-Q03-002
- 状态: 待审核
- 严重级别: Major
- 位置: `WtRdmDtReader.cpp:1186/:1281/:1479/:1586`、`:308`、`:458`；同型 `WtDataReader.cpp:423/:557/:714/:871`
- 描述: 这些调用点全部**忽略 `proc_block_data` 的返回值**。该函数在 `BLOCK_VERSION_CMP/CMP_V2` 时做
  `content.size() != sizeof(BlockHeaderV2) + blkV2->_size` 长度校验并 `return false` 且**不改写 content**；
  调用方随后仍按 `content.size()/sizeof(结构体)` 当记录条数用 → 把「压缩流字节」当 bar/tick 解析，静默产出脏数据。
- 触发输入: 声明 `_size` 与真实文件长度不一致的压缩 `.dsb`（例如截断的压缩库文件）。
- 期望: `if(!proc_block_data(...)) { 记 LL_ERROR 并放弃该文件 }`。
- 说明: 与 BUG-L04-002（proc_block_data 未校验最小长度）是同一族，但这条是**调用侧丢弃返回值**，原清单未覆盖。

## BUG-Q03-003
- 状态: 待审核
- 严重级别: Minor
- 位置: `WtRdmDtReader.cpp:1180/:1275/:1381/:1473/:1580`、`:301/:451`；`WtDataReader.cpp:416/:558/...`
- 描述: 长度校验统一用 `sizeof(HisKlineBlock)`/`sizeof(HisTickBlock)`（= `BlockHeader` 12 字节），
  但 writer 侧压缩文件用的是 `BlockHeaderV2`（20 字节，见 `WtDataWriter::dump_day_data`，p10 已实测 `_size == filesize-20`）。
  于是 [12, 19] 字节的 V2 残骸文件能过校验，`proc_block_data` 再把 12 字节当头擦掉（`content.erase(0, BLOCK_HEADER_SIZE)`，
  非压缩非老版本分支）→ 载荷里混进 8 字节 `_size` 字段被当成记录首字段解析。
- 期望: 按 `_version` 选择 `BLOCK_HEADER_SIZE` / `BLOCK_HEADERV2_SIZE` 做剥头与最小长度校验。
- 备注: 与 BUG-P10-004（只有头部的残骸文件）相邻，本条补的是「V2 头 + 12 字节擦头」的错配。

## BUG-Q03-004
- 状态: 待审核
- 严重级别: Minor
- 位置: `WtRdmDtReader.cpp:324/:487`、`WtDataReader.cpp:434`
- 描述: 两处无意义/易错的边界写法：
  1. `uint32_t tcnt = ...; if (tcnt <= 0) break;` —— 无符号 `<= 0` 恒等价于 `== 0`，属误导性比较；
  2. `std::lower_bound(tBlock->_ticks, tBlock->_ticks + (tcnt - 1), ...)` 把**上界写成 `tcnt-1`**，
     末条记录永远不参与右边界定位（只能靠 `eIdx+1` 兜回来）。当全部记录都早于目标时间时结果恰好正确，
     但只要「末条恰好等于目标时间」语义就与闭区间预期不一致，后续维护极易踩坑。
- 期望: 写 `if (tcnt == 0)`，并用 `std::upper_bound(..., tBlock->_ticks + tcnt, ...)` 或显式注释保留 `-1` 的理由。

## BUG-Q03-005
- 状态: 待审核
- 严重级别: Minor
- 位置: `WtRdmDtReader.cpp:2643`（`clearCache`），配合 `:269/:299/:449` 的 `_his_tick_map`、`:2187` 的 `_rt_min1_map`
- 描述: `clearCache()` 只清 `_bars_cache`（K 线缓存）与 `_adj_factors` 保留，**不回收**
  `_his_tick_map` / `_his_ord*_*map` / `_rt_*_map` 里按「整文件读入 `std::string`」缓存的历史块缓冲。
  长时间运行（如回放/多日查询）后，内存只随「读过的交易日数」单调增长，唯一回收点是进程退出。
- 触发证据: `q03_rdm_kline_file_test.cpp::day_missing_file_caches_empty_entry` 与
  `clear_cache_forces_rebuild_from_file` 观测到的日志计数差（tick 侧缓存改文件后不重载）。
- 期望: `clearCache()` 一并 `clear()` 五张 his 表与五张 rt 表（rt 表需先解除 mmap）。

## 引用（不重复登记）
- BUG-P09-001 / BUG-P05-007：`indexBarFromCacheByRange/ByCount` 的 `etime==0` uint32 回绕与 `eit--` 缺守卫。
  本波**未从公开入口复现**：`readKlineSliceByRange`（`:1807`）会把 `etime==0` 归一为 `203012312359`，
  缺陷只在直接调用私有函数时暴露，保持「待审核」不变。
- BUG-P09-002：`readBarsFromCacheByRange` 缺 `_bars.empty()` 守卫；本波公开路径未触达（`indexBar*` 侧有守卫）。
- BUG-P09-003：`WtDataReader::init` 未判 `cfg->get("path")` 返回值即 `isArray()`。**本波用例必须提供 `path`** 才不踩空指针，
  已在 `q03_reader_file_test.cpp` 注释中固化该前提。
- BUG-L04-003：`WtBtDtReader` 五个 `read_raw_*` 未校验文件长度。
