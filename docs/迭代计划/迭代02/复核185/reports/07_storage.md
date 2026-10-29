# 07_storage 第二轮复核

## 汇总

8 项均重新对照原记录、当前实现、公开接口/调用者和测试前置：**PRODUCT 8、DUPLICATE 0、TEST 0、NOT_BUG 0、NEEDS_DECISION 0、EXTERNAL_BLOCKED 0、LOCAL_INCOMPLETE 0**。证据等级为 **LOCAL_REPRO 3、STATIC 5**。本组没有需要产品语义决策或外部 SDK/平台才能裁定的项；非法外部输入问题按安全拒绝缺陷裁定，不等同于该输入是有效业务数据。

主代理刷新后的 `CoverageTests` SHA-256 为 `e46841019880f543999fc3a47a2ab9747c669cc628452b64cd04de77cc8949f6`。本轮 6 个定向单例各在独立进程、临时 cwd、LLVM profile 与 XML 下运行，timeout 15 秒：3 PASS、3 FAIL。R23 与 P10 的当前测试源已先于该目标刷新而改为回归正确行为；以下 FAIL 是该正确预期被当前产品实现违反，不是旧测试和新测试结果混版。二进制、目标对象与测试源哈希/mtime 记录在 `evidence/07_storage/post-refresh-*`；命令、返回码、XML 计数和原始 stdout 在 `evidence/07_storage/runs/20261029T110035-2ce0cd4f-manifest.json` 及其单例目录。

## PRODUCT

### BUG-R1-003：公开交易日查询对缺段 PID 越界

**结论：PRODUCT（STATIC，限输入校验/安全拒绝）。** `IBaseDataMgr::isHoliday`、`calcTradingDate` 等公开方法接收 `pid`；当 `isTpl=false` 时 `getTplIDByPID` 用 `split(pid, ".")` 后无条件读取 `ay[0]`、`ay[1]`。单段字符串会使 `ay[1]` 越界。该问题只主张 malformed PID 进入公开 API 后未安全拒绝，不把 `NOPIVOT` 登记为有效品种代码。weekday 的公开调用会走到解析分支；周末提前返回不能覆盖此触发条件。现有单段用例仍是 disabled，未在主宿主启用，也未作 ASan 复现。

**修复/验收：** 在 split 结果访问前验证段数、空段及必要的格式；不合法时返回明确失败值/空模板 ID并记录可诊断日志，不触发数组越界。对 `isHoliday` 的周一 malformed 输入断言安全返回；保留 `SSE.STK` 正常映射和未知但有完整两段 PID 的对照。安全回归应在产品修复后启用；如需确认当前 UB，使用主代理安排的有界子进程/sanitizer 探针。

### BUG-R23-004：短历史文件失败后空缓存项阻止重读

**结论：PRODUCT（LOCAL_REPRO）。** ByCount 先用 `_his_tick_map[key]` 插入默认项，再读文件。块尺寸不足时只清 `_buffer` 并退出，条目仍在 map 中；后续调用由 `find` 命中并进入 `bHasHisTick` 路径，读到 `_block == NULL` 即退出，不重新打开文件。

这不是仅靠“运行中任意替换历史文件”推导出的需求：正常 writer 通过 `create_new_file` 使目标文件先可见，再依次写 header 和 payload，随后关闭；写调用返回 bool 但调用点未检查，也没有临时文件加原子 rename。读者与创建/写入并发时可以先看见短文件。首个长度失败后，writer 完成写入也不能使该 reader 再读到数据。修复应在所有读取和长度/解码失败路径中不缓存失败项，或 erase 对应 key；不要保留 `_block == NULL` 的命中项。

**定向证据：** 当前源版本的 `CountSizeCheckFailureCanRecoverAfterFileRepair` 在本轮运行 FAIL。首次读取后用同一 reader 写入合法 20240108 文件并重查，实际仍返回 20240107 的 330/332，而不是修复文件中的 130/132。测试已由旧的“失败项永远存在”现状断言改成同 reader 恢复契约；本轮 fresh target 编译并运行的是新测试版本。ByDate/ByRange 有同类插入/尺寸失败模式，尚未分别动态覆盖。

**修复/验收：** `readTickSliceByCount` 的失败分支清除并移除失败 key；同一策略应用于 ByDate/ByRange。用一个失败短文件首读、文件完成/修复后同一 reader 重读，确认回到该日、数据和错误日志正确；另验证不存在文件的正常空结果、合法历史文件一次加载。分别复核三种入口。

### BUG-L04-004：外部合约配置标识长度越界写 LMDB key

**结论：PRODUCT（STATIC，外部配置输入验证）。** `loadContracts` 从外部合同 JSON 的交易所/代码字段创建 `WTSContractInfo`，再由 AD writer 将标识用于 LMDB key。key 固定 `_exchg[16]`、`_code[32]`，构造时 `strcpy`，且没有长度验证。外部 exchange 长度 16–31 虽能装入上游 32 字节字段，仍会越过 key 的 exchange 数组并覆盖后续字段；code 超过 31 字符则可能在 `WTSContractInfo::create` 更早由 `wt_strcpy` 越界。`wt_strcpy` 只按输入长度 `memcpy`，再写终止符，没有目标容量参数。

**修复/验收：** 在 `WTSContractInfo::create` 前的合同导入边界校验 exchange/code 长度，使任一层固定数组都能容纳终止符；不允许静默截断造成 key 碰撞。验证最大合法长度可载入和 roundtrip；分别输入 exchange 16 字符、code 32 字符应安全拒绝，不注册合同也不写 LMDB key。当前 `MaxLengthFieldsFitExactly` 正常最大值用例 PASS；未在主进程调用超长构造函数。

### BUG-L04-005：AD K 线 reader 将任意 LMDB value 直接当完整 bar

**结论：PRODUCT（STATIC，限持久化输入边界）。** `WtDataReaderAD` 实现公开 `IDataReader::readKlineSlice`。首次加载的 `cacheBarsFromStorage` 对 query 返回的 value 直接 cast 并复制一个 `WTSBarStruct`；增量 `update_cache_from_lmdb` 也直接 cast 后读取日期/时间、复制结构体，没有比较 `item.size()`。标准 writer 的 `pipeToDayBars/pipeToM1Bars/pipeToM5Bars` 每条写入 `sizeof(WTSBarStruct)`，所以不声称标准 writer 会产出短值；配置路径里的持久化 LMDB 是 reader 的外部文件输入，短/旧/损坏 value 必须被拒绝而不能形成越界读。

候选关联的 `GetUppersSkipsBarEqualToLastBarTime` 只直接调用 `WtLMDBQuery` 并在测试里 cast value，未调用 `WtDataReaderAD::readKlineSlice`，不是短 value 安全证据。候选提到的 update 日志 `.back()` 也不能单独成立：公开 `readKlineSlice` 在首次加载后若 `_bars` 为空会于 `:419-420` 先返回；若进入 update，初始缓存已有 bar，update 只替换/追加，不清空列表。因此本项保留独立短 value 风险，不把 `.back()` 写成已证实缺陷。

**修复/验收：** 初次加载和增量 query 的每条 value 均在 cast 前验证 `size == sizeof(WTSBarStruct)`；无效值记录数据库 key/长度，停止该条处理并返回稳定错误/空结果，不能向 sink 回调垃圾 bar。通过 public `readKlineSlice` 分别覆盖短值拒绝和标准 writer 尺寸控制，并覆盖 cache 更新路径。当前没有运行短值 fixture，以免在无 guard 的实现上触发 UB。

### BUG-P09-003：非空 reader 配置缺 path 时解引用空指针

**结论：PRODUCT（STATIC，外部配置安全拒绝）。** `IDataReader::init` 是插件公开接口，行情与回测管理器把配置传给 reader。`WtDataReader::init` 对 `cfg == NULL` 有提前返回，但对非空 cfg 无条件 `cfg->get("path")->isArray()`；缺 path 返回空指针会在 `:162` 解引用。空数组虽不会空指针，但不会加入目录，随后 `:204` 索引 `_rt_dir[0]`、`_his_dir[0]` 也越界。配置字段必填与否不改变应对 malformed config 安全失败的要求。

**修复/验收：** 检查 path 存在、类型可解析且目录列表非空；失败时记录字段级错误、保持 reader 不可读取的显式状态，并在 read 方法入口安全返回，不使用 `[0]` 空索引。将旧 disabled crash 用例改为启用的安全拒绝断言；覆盖缺失/null/空数组配置，并保留一个合法最小 path 对照。当前未运行缺 path 测试。

### BUG-P10-003：writer block decoder 不校验文件 magic

**结论：PRODUCT（LOCAL_REPRO）。** 格式定义把 `_blk_flag` 注释为数据块标识，writer 创建各类块时写 `BLK_FLAG`。`WtDataWriter::proc_block_data` 直接按版本字段走 RAW/压缩解析，没有验证 magic；writer 的正常续写流程会读已有历史文件并调用该方法。错误 magic 的 RAW_V2 输入会被当作有效块，`bKeepHead=false` 时直接剥掉 header，调用者继而按有效 bar 内容处理/回写。这里仅要求校验通用 magic；该 helper 没有 expected block type 参数，不能要求它单独判定具体 `_type`。

**定向证据：** 当前 `BlockFlagMismatchRejectsWithoutMutatingContent` 是 worker 已修正后的源版本并已重建运行；本轮 FAIL：函数实际返回 true，且删除了 12 字节错误 magic 头，与拒绝且内容不变断言相反。原来的 `BlockFlagNotValidated` 测试把当前实现行为当作预期；现已改为格式标识安全断言，测试与产品二进制是同一新版本。

**修复/验收：** 完成最小头长校验后比对 `BLK_FLAG`；不匹配时在解码前返回失败且不改内容。检查 `dump_day_data` 等续写调用点确实中止当前文件更新，而非忽略失败后继续拼接/回写。验证合法 RAW/CMP 及旧结构兼容控制仍通过；再运行当前错误 magic 回归，断言 false、原内容不变。

## PRODUCT

### BUG-R23-005：ByCount 历史分支丢失与 etime 相等的 tick

**结论：PRODUCT（LOCAL_REPRO）。** `IDataReader::readTickSlice` 将 `etime` 描述为结束时间，并以 0 表示读到最后一条。Rdm ByCount 的 RT 分支只在记录严格晚于 etime 时回退，保留完全命中 etime 的 tick；history 分支对 `>= etime` 回退，恰好命中也会回退一条。当前三条数据 fixture 以 14:31:000 为 etime：RT 对照测试 PASS，history 测试 FAIL，并从前一交易日补入数据。测试源已在刷新 CoverageTests 目标前改为正确的边界预期；本轮失败是当前产品实现与该预期不符，旧测试执行记录与新测试源并非同一版本。

**独立根因与交叉关系：** 问题位于 `WtRdmDtReader::readTickSliceByCount` 内部独立的 RT/history count 选择及跨日补数路径。`BUG-Q03-004` 检查的是 `readTickSliceByRange` 的区间端点回退，API 和实现路径不同；两项表现为同类比较器边界模式，但互不共享比较器，任一修复都不能替代另一项的修复或验收。

**修复/验收：** 在 `readTickSliceByCount` 中单独统一 RT/history cutoff 比较条件，使 `tick_time == etime` 在 count 选择和跨日补数前保留。使用同一 3 tick 输入分别放置 RT block 与 history file，count 请求命中中间 tick 时，两分支均返回截止记录及之前最近 count 条，不得因 history 丢边界再从前日补足；另覆盖 etime 命中块内末条。该测试和修复独立于 `BUG-Q03-004` 的 range 验收。

### BUG-Q03-004：history range 分支排除与 etime 完全相等的 tick

**结论：PRODUCT（STATIC；范围端点的动态补测尚缺）。** `readTickSliceByRange` 的 history 分支在 lower_bound 后对 `pTick->action_time >= eTick.action_time` 回退；RT 分支则只对 `>` 回退。接口将 etime 描述为结束时间，故截止边界应包含与 etime 相等的最后一笔；相同 ticks 落在历史文件还是当前 RT block 不应改变查询结果。`tcnt` 为无符号数，`tcnt <= 0` 等价于 `tcnt == 0`，仅为写法问题，不纳入产品缺陷。`BUG-R23-005` 的 count API 有独立实现；它提供了同类边界差异的动态旁证，不能作为本 range 入口的复现。

现有 Q03 `range_clips_right_window_on_end_trading_date` 只确认严格晚于 etime 的 tick 被裁掉，本轮 1/1 PASS；它没有一条精确等于 etime 的记录，也没有测试 history 精确边界。因此本项仍为 STATIC。

**修复/验收：** 在 `readTickSliceByRange` 中单独统一 RT/history range 的端点回退条件，使 `tick_time == etime` 纳入、`tick_time > etime` 排除；保留早于首条和单条块的边界防护。给 Q03 公共 range 新增 RT 与 history 等时测试，同一 fixture 还应验证严格晚于边界不返回。R23 count 需通过自身的独立修复和回归，不得用它代替本 range API 验收。

## 本轮运行结果

| 用例 | 结果 | 用途 |
|---|---:|---|
| `R23RdmCountTest.HisWindowBoundaryKeepsTickExactlyAtEtime` | FAIL | 历史 count 丢失等时 tick，回补到前日 |
| `R23RdmCountTest.RtWindowBoundaryKeepsTickExactlyAtEtime` | PASS | RT 对照保留等时 tick |
| `R23RdmCountTest.CountSizeCheckFailureCanRecoverAfterFileRepair` | FAIL | 同 reader 修复文件后仍被失败缓存项屏蔽 |
| `P10ProcBlockTest.BlockFlagMismatchRejectsWithoutMutatingContent` | FAIL | 错误 magic 被接受并剥除 header |
| `Q03RdmTickTest.range_clips_right_window_on_end_trading_date` | PASS | 只证明严格晚于 etime 被裁剪，不覆盖等时记录 |
| `L04LmdbKeys.MaxLengthFieldsFitExactly` | PASS | 只证明受支持最大值正控，不覆盖超限拒绝 |

## 限制与待续动作

本轮没有直接运行 R1 malformed PID、P09 缺 path、L04 超长 key 或 L04 短 LMDB value：前三类中的越界/空指针用例不在主测试宿主直接启用，短 value 在尚无长度守卫的 reader 中可能触发未定义行为。R1 若需要动态确认，按主代理安排在有界子进程/sanitizer 探针中完成。未运行 ASan/UBSan。R23-004 的 ByDate/ByRange、Q03 range 精确等时、L04 真实 public reader malformed-value 路径还需后续目标测试与产品修复后复验；不把本轮局部测试外推为整组完成的代码修复。
