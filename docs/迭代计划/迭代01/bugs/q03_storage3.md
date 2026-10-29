# q03_storage3 疑似缺陷清单（第五波 lane q03，WtDataStorage 读侧）

- 车道: q03（`src/TestUnits/coverage/q03_storage3/`）
- 生产代码改动: 无。以下只记录，不修源码。
- 与既有清单的关系: BUG-P09-001/002/005、BUG-P10-004、BUG-L04-002/003 同源项**引用原编号**，不重复登记。

> 复核注记（2026-10-29）：各条以自身证据边界为准；Q03-001 已补充本机隔离运行，其他条目不能因测试文件存在而视为运行复现。

---

## BUG-Q03-001
- 状态: 本机隔离回归 exit 139；未取得崩溃栈
- 严重级别: Major
- 位置: `src/WtDataStorage/WtRdmDtReader.cpp:1189/:1194/:1196`（`cacheHisBarsFromFile` 主力连续分支），同型见 `:1431/:1435/:1437`（股票复权分支）
- 描述: `proc_block_data(content, true, false)` 后按整根数量计算 `barcnt`、resize vector，却以原载荷字节数 `memcpy`。RAW_V2 脱头后 50 字节不足一根时，`barcnt==0`，先向零大小 vector 的 data 拷贝非零字节，已构成越界写；之后还无条件访问 `hotAy->at(barcnt - 1)`。对于含完整 bar 加残余字节的载荷，拷贝长度同样超出所分配的 bar 容量。此前将 `out_of_range` 写成必然观察结果忽略了更早的未定义行为；异常路径也可能泄漏裸 `new` 的 vector。
- 触发输入: `his/day/<exchg>/<exchg>.<product>_<ruletag>.dsb` 大小落在 [12, 99] 且版本位为 `BLOCK_VERSION_RAW_V2`。
- 复现: 修正 HOT 合约切段和 YYYYMMDDHHMM 时间输入后，`Q03RdmTickTest.DISABLED_his_kline_hot_fragment_returns_null` 独立运行 exit 139；有效日线文件对照 `Q03RdmKlineTest.day_dsb_raw_v2_full_range_roundtrip` PASS。未取得崩溃栈，不能把具体故障指令或信号归因写成调试器实测。
- 期望: `barcnt == 0` 时视为空文件跳过，并释放 `hotAy`。
- 修复方向: 在分配、拷贝和读取末条之前验证解码成功、载荷非空且为完整 `WTSBarStruct` 的整数倍；用 RAII 管理 `hotAy`，坏文件记录错误并跳过。修复后启用回归，并覆盖空载荷和完整记录后附残余字节。

## BUG-Q03-002
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDataStorage/WtRdmDtReader.cpp:1186/1281/1479/1586`；`src/WtDataStorage/WtDataReader.cpp:1041/1172/1352/1435/1675`
- 描述: 这些调用点全部忽略 `proc_block_data` 的返回值。该函数在 `BLOCK_VERSION_CMP/CMP_V2` 长度不等于 `sizeof(BlockHeaderV2)+_size` 时返回 false 且不改写 `content`；调用方仍可能计数、cast、swap 或把原 buffer 交给上层。因此错误压缩流可能被当 bar 数据处理，但具体后果因调用点不同，不能统一声称都静默产出脏记录。
- 触发输入: 声明 `_size` 与真实文件长度不一致的压缩 `.dsb`（例如截断的压缩库文件）。
- 期望: `if(!proc_block_data(...)) { 记 LL_ERROR 并放弃该文件 }`。
- 复核裁定: 调用侧丢弃失败返回是独立于解析器未校验最小头长（BUG-L04-002）的根因，Q03-002 应独立保留；L04-003 仅为 L04-002 的入口表现，不重复计数。本轮只有源码证据，没有运行损坏文件复现。
- 修复方向: 每个调用点检查返回值，失败时记录路径并放弃该文件；增加 CMP_V2 `_size` 与真实文件长度不符的调用侧测试。

## BUG-Q03-003
- 状态: 已合并（BUG-L04-002）
- 严重级别: 并入 BUG-L04-002，不单独定级
- 位置: 解析器 `src/WtDataStorage/WtDataReader.cpp:68-70`；只做 12 字节预检的 caller 位于 `WtRdmDtReader.cpp:1180-1186/1275-1281/1473-1479/1580-1586` 与 `WtDataReader.cpp:1034-1041/1165-1172/1345-1352/1428-1435/1665-1675`
- 描述: 原表述混淆 RAW_V2 与压缩 V2 头。writer 生成 RAW_V2 使用 12 字节 `BlockHeader`（`WtDataWriter.cpp:1894-1900`）；20 字节 `BlockHeaderV2` 的 `_size` 属 CMP/CMP_V2 压缩格式。12-19 字节 CMP_V2 文件确会通过调用方的 12 字节预检，但解析器在 `WtDataReader.cpp:68-70` 读取 V2 `_size` 后才比较实际长度；这属于 BUG-L04-002 的版本相关最小头校验缺失。
- 期望: `proc_block_data` 在读取 `_size` 前，按压缩版本要求至少 20 字节；RAW_V2 仍按 12 字节头解析。
- 复核裁定: “RAW_V2 的 12~19 字节残骸含 `_size` 被擦头后解析”不成立。Q03-003 并入 BUG-L04-002，不单独计数；调用方忽略 false 的问题另见 BUG-Q03-002。
- 修复方向: 按版本先校验头长，再解析；修正文案并分别覆盖 12 字节 RAW_V2 与截断 CMP_V2 输入。

## BUG-Q03-004
- 状态: 待审核
- 严重级别: 待验证（接口区间契约未确认）
- 位置: `WtRdmDtReader.cpp:324/:487`、`WtDataReader.cpp:434`
- 描述: 两处代码写法可读性不佳，但目前不能据此确认产品行为错误：
  1. `uint32_t tcnt = ...; if (tcnt <= 0) break;` —— 无符号 `<= 0` 恒等价于 `== 0`，属误导性比较；
  2. `std::lower_bound(tBlock->_ticks, tBlock->_ticks + (tcnt - 1), ...)` 没把末条放入搜索范围，随后使用 `eIdx`/`>= eTick` 处理边界。该形式可能是为了避免返回 end；目前没有 API 证据证明 etime 必须包含等于末条的记录。
- 复核裁定: `tcnt<=0` 对无符号数恒等于 `tcnt==0`，只是写法问题；末条端点应包含还是排除需先确定接口区间约定。当前证据不足以按产品缺陷定级。
- 修复方向: 明确上界闭/开区间契约，并加首条前、末条等于上界、末条晚于上界、单记录测试，再按契约选择 `lower_bound`/`upper_bound`。早于首条时 `eit--` 的 UB 属 BUG-P05-007/P09-001，不并入本条。

## BUG-Q03-005
- 状态: 源码确认与具体类清缓存契约不符；动态复现未运行
- 严重级别: 待定（历史缓存未清；实际内存增长未量测）
- 位置: `src/WtDataStorage/WtRdmDtReader.cpp:2643-2654`（`clearCache`），history map 声明见 `WtRdmDtReader.h:266-269`；逐日 key 见 `WtRdmDtReader.cpp:267/416/699/874/1048`。
- 描述: 当前 `clearCache()` 清 `_bars_cache` 与六张 RT map（`_rt_min1/_rt_min5/_rt_tick/_rt_trans/_rt_orddtl/_rt_ordque`），不清四张 history map（`_his_tick/_his_orddtl/_his_ordque/_his_trans`）。历史 HFT map key 含交易日，长寿命 reader 查询不同日期会保留不同整文件 buffer；源码能证明保留行为，不能单凭代码断言实际 RSS 已增长到不可接受。
- 触发证据: 原引用的 `q03_rdm_kline_file_test.cpp::day_missing_file_caches_empty_entry` 和 `clear_cache_forces_rebuild_from_file` 只测 K 线缓存，不触碰 HFT history maps；撤回其作为 tick 缓存/RSS 证据的说法。本轮未运行内存增长复现。
- 期望: `WtRdmDtReader.h:485-490` 明确说明清除所有缓存，包含 Tick、委托明细、队列和成交；清理后不得继续命中清理前的历史缓存。长期运行的内存上界需另行定义。
- 复核裁定: 类自身的公开注释与实现不一致，四张历史 map 被遗漏是源码层面的缺陷；此处未运行历史文件读取与清缓存回归，也未量测 RSS 或证明无界增长。原“5 张 his + 5 张 rt”数量不准确，当前为 4 张 history 与 6 张 RT map。
- 修复方向: 在确保没有并发读者和悬垂切片的生命周期前提下清理四张 history map；补充读取历史文件、清缓存、改写或移走文件后重新读取的行为测试。若需长期自动淘汰，另行设计策略并验证 N 日后的内存上界。

## 引用（不重复登记）
- BUG-P09-001 / BUG-P05-007：`indexBarFromCacheByRange/ByCount` 中 `eit==begin()` 时缺少回退守卫。公开 `readKlineSliceByRange` 只把 `etime==0` 归一化（`:1807-1808`）；明确传入非零且早于首根缓存 bar 的 end 仍能进入索引函数，因此“仅私有入口可触发”不成立。本轮未运行复现，应合并为一个 canonical 条目。
- BUG-P09-002：`readBarsFromCacheByRange` 缺 `_bars.empty()` 守卫；本波公开路径未触达（`indexBar*` 侧有守卫）。
- BUG-P09-003：`WtDataReader::init` 未判 `cfg->get("path")` 返回值即 `isArray()`。**本波用例必须提供 `path`** 才不踩空指针，
  已在 `q03_reader_file_test.cpp` 注释中固化该前提。
- BUG-L04-003：`WtBtDtReader` 五个 `read_raw_*` 未校验文件长度。
