# p10_storage_writer 疑似缺陷（状态：待审核，未改任何生产代码）

对象文件：`src/WtDataStorage/WtDataWriter.cpp`（2701 行）。行号来自本轮 Read 实测。`DISABLED_` 只表示默认不运行，不表示缺陷已复现；本轮逐项复核见文末。

## BUG-P10-001（Major 候选）`proc_block_data` 在读取块头前未校验长度
- 位置：`WtDataWriter.cpp:1818-1844`
- 现象：先把 `content.data()` 强转为 `BlockHeader*` 并读 `_version`，没有先验证至少有
  `sizeof(BlockHeader)` 字节；若版本是 `BLOCK_VERSION_CMP` 或 `BLOCK_VERSION_CMP_V2`，随后还把同一地址
  强转为 `BlockHeaderV2*` 并读 `_size`，故需先保证至少有 `sizeof(BlockHeaderV2)` 字节。
  空串/不足 12 字节和 12–19 字节压缩头都可能触发越界访问/未定义行为，但不保证一定以段错误表现。
  长度相等检查通过后，`_size` 与缓冲区 payload 长度相等；现有证据不支持原文“短头会继续解压并越界读”的断言。
- 与 P05-006 的关系：P05-006 是 AD reader 从 LMDB 定长 value 拷贝结构体，代码路径不同，不是同一缺陷。
- 证据边界：源码可证没有头长 guard。原 `DISABLED_CrashClassInputs` 的 16 字节字符串被写入 20 字节头，
  fixture 本身无效；现已替换为三个独立子进程用例，仅写入实际存在的字节。空串和 4 字节输入未满足
  “返回 false 且内容不变”的组合断言，子进程退出码均为 2；16 字节压缩头及合法 RAW_V2 对照通过。
  普通构建未运行 ASan，不能据此证明短头访问安全，组合退出码也不能区分错误返回与内容变化。
- 修复方向：先检查基础头长；识别压缩版本后再检查完整 V2 头长，并用无溢出的方式验证声明长度与实际 payload
  一致，失败返回 `false` 且不改写 `content`。保留独立的短输入安全拒绝断言，并用 sanitizer 复核。

## BUG-P10-002（测试误报候选）老版压缩块无法解码
- 状态: 已撤回（原测试格式前提不成立；其它历史格式待样本验证）
- 位置：`WtDataWriter.cpp:1822-1841`
- 原登记把所有 `BLOCK_VERSION_CMP` 旧结构压缩文件都描述成 12 字节头。当前仓库可见格式证据并不支持这个前提：
  `DataDefine.h` 的 `BlockHeaderV2` 为压缩长度增加 `_size`；测试 fixture `make_block` 对 `BLOCK_VERSION_CMP`
  与 `BLOCK_VERSION_CMP_V2` 都写 `BlockHeaderV2 + 压缩 payload`；`q03_rdm_kline_file_test.cpp` 的
  `day_old_struct_versions_are_upgraded` 使用该格式成功读回旧结构体。
- 现有启用用例 `P10ProcBlockTest.OldCompressedFlagGoesThroughV2HeaderCheck` 不是有效旧压缩块：它调用
  `make_raw_bar_block(BLOCK_VERSION_CMP, 3)`，实际组装成 12 字节基础头 + 未压缩的新结构体。V2 长度校验拒绝此
  输入并不证明合法 CMP 文件无法解码；`WtDataWriter::proc_block_data` 的静态路径会解压 payload，再因旧版本标志
  转换结构体。
- 复核裁定：**当前“永远无法解码”的结论属于测试格式误报，不据此认定产品缺陷。** 是否还需支持一种不含 `_size`
  的 12 字节 CMP 历史格式，仓库内未找到格式说明或真实样本，仍待格式契约确认。
- 最小测试修正：构造 `BlockHeaderV2`（`_version=BLOCK_VERSION_CMP`、`_size=压缩长度`）并压缩
  `WTSBarStructOld` payload，直接测试 `WtDataWriter::proc_block_data` 能否解压并升版。若外部旧格式确实是
  12 字节头，先补真实样本/格式规范，再单独增加兼容分支；不能用 12 字节头 + raw payload 代表它。
- 后续验证（2026-10-29）：已将该用例改为合法 `BlockHeaderV2 + 压缩旧结构`，重新构建并运行 `P10ProcBlockTest.OldCompressedBarBlockDecompressesAndUpgradesStruct`，PASS，逐字段验证三根 bar。

## BUG-P10-003（Minor，格式契约待确认）`proc_block_data` 不校验块标志 `_blk_flag`
- 位置：`WtDataWriter.cpp:1818-1831`（全函数无 `BLK_FLAG` 比对）
- 实际行为：原用例以 `"WRONGFLG"` 替换 magic 后按现状断言返回 true；现改为 `DISABLED_BlockFlagValidationContractPending`，不把接受或拒绝当作已确认合同，未运行；
  源码只看版本位并按调用参数去头/解压。`DataDefine.h` 将 `_blk_flag` 注释为数据块标识，writer 写块时也填
  `BLK_FLAG`，但当前接口/仓库文档没有明确规定 decoder 必须拒绝错误标志；测试按现状断言，不能单独证明期望契约。
- `_type` 与 magic 分开处理：该函数没有期望 `BlockType` 参数，因而不能据此要求它判定某个具体文件类型；调用方
  若需要校验类型，应在知道预期类型的边界做检查。原登记将“错误类型也接受”并列成此函数缺陷，证据不足。
- 复核裁定：**静态行为已确认；是否为产品缺陷待格式校验契约确认。** 与 AD 缓存头校验条目属于相近格式健壮性主题，
  但不是同一代码路径，不宜合并为同一具体缺陷。
- 修复方向：若格式规范要求 magic 必须匹配，则在长度检查之后、读取版本分支之前拒绝错误 `_blk_flag`；由调用方
  校验所需 `_type`。相应测试应断言格式规范，而不是把当前接受行为当作期望。

## BUG-P10-004（Major 候选）只有头部的历史 `.dsb` 会静默丢弃新 bar
- 位置：`WtDataWriter.cpp:1951-2006`
- 现象：`dump_day_data` 走「文件已存在」分支时，`barcnt = content.size()/sizeof(WTSBarStruct)`；
  若文件只有 12 字节头部（前次写入被中断/截断的典型残骸），`barcnt == 0` 使
  `if (barcnt != 0)` 的追加分支整体跳过，随后仍 `truncate_file(0)` + 只重写头部 → 本次 `newBar` 丢失，
  而函数返回 `true`（`bNeedWrite` 被标 `[[maybe_unused]]`，从未参与判断，`:1936`）。
- 后果：日线断点后无法自愈，需人工删文件；调用方（`dump_bars_to_file`）看不到失败。
- 证据边界：源码与启用用例 `P10DayDsbTest.EmptyHistoryFileWithHeaderOnlyDecodesToEmptyPayload` 证实当前
  `dump_day_data` 返回 true 且文件仍只有 12 字节；本轮单跑该用例 PASS（PASS 表示现状断言成立，不是产品验收通过）。
  用例构造的是可解码为零长度 payload 的 RAW_V2 header-only 文件；真实掉电/截断现场未单独模拟。
- 复核裁定：**给定现有文件和新 bar 输入，静默未持久化是已确认行为；按 `dump_day_data` 返回成功却丢掉本次新 bar 的语义，属于产品缺陷。** 是否将损坏文件当空历史自动恢复可由格式策略决定，但必须显式返回失败或保留本次数据，不能静默成功。
- 最小修复方向：对 header-only 零 payload 明确走空历史写入分支，把 `newBar` 落盘；若文件被判为损坏则保留/隔离原文件并返回失败。测试拆分“空 payload 可解码”与“dump 新 bar 后确实持久化”两项断言。
- 后续验证（2026-10-29）：已拆分为 `HeaderOnlyDayFileDecodesToEmptyPayload` 和 `AppendingToHeaderOnlyDayFileMustPersistBarOrRejectWithoutMutation`；重建后前者 PASS，后者 FAIL：写入返回 true，但文件仍只有 12 字节，期望有完整 bar 时为 100 字节。

## BUG-P10-005（Major 候选）主存储 writer 的异步队列使用锁外判空与无谓词 wait
- 更正对象：`WtDataWriter` 没有 `_async_task` 字段，成员是 `_async_proc`（`WtDataWriter.h:503`），由
  `init()` 从参数读取（`WtDataWriter.cpp:131`）。因此“`_async_task` 未初始化”是把 `WtDataWriterAD` 的
  BUG-P05-005/BUG-R4-BUG-01 混到主存储 writer 的误植；主 writer 在未 `init` 就写入时的调用契约未确认，不能当成
  正常生命周期缺陷。
- 主 writer 仍有两个需要分开描述的队列风险：`pushTask` 的 `_tasks` 在 producer 入队时持 `_task_mtx`，但 worker
  在 `WtDataWriter.cpp:592` 锁外检查 `_tasks.empty()`，之后才加锁并无条件 wait（`:594-595`）。入队通知可能发生在
  锁外判空之后、worker 真正 wait 之前，worker 随后会带着已有任务睡眠，直到下一次通知；锁外 `empty()` 也构成并发访问。
  `proc_loop` 的 `_proc_que` 在收盘入口（`:1595-1604`）入队时未用 `_proc_mtx`，worker 同样锁外判空后无谓词 wait
  （`:2220-2224`），队列读写还缺少共同互斥保护。后者不依赖 `_async_proc`。
- `check_loop` 的 10 秒轮询和清理路径的 300 秒异常重试不是上述条件变量竞态的证据，应与本条分开；R172 只记录
  正常清理路径运行边界，没有对竞态做动态复现。P05-005 的 AD writer 是另一类对象：同属队列/线程风险家族，代码路径
  不同，不能重复计数为同一具体实现缺陷。
- 复核裁定：**队列同步问题由源码静态条件确认，动态挂起未复现；未初始化字段的原说法为误报。** `release()` 当前
  只 join `_proc_thrd`，`_task_thrd` 停止/回收契约也应单独审核；本轮没有据此声称已发生退出挂死。
- 最小修复方向：每个队列的检查、入队、出队均使用同一 mutex，并采用 `wait(lock, predicate)`（含队列非空/终止条件）；
  producer 入队后通知，release 设置终止条件、唤醒并 join 相应 worker。用可控并发测试覆盖空队列唤醒、连续入队和退出。

---

## 本轮独立复核裁定（2026-10-29）

| 编号 | 裁定与证据边界 | 最小修复方向 |
|---|---|---|
| BUG-P10-001 | **短头解析的条件性产品缺陷。** 读 `BlockHeader` 前无 12 字节检查，压缩版本读 V2 `_size` 前无 20 字节检查。现用独立禁 core 的子进程验证 false 且内容不变：空串及 4 字节输入 FAIL（子进程以 2 退出，未满足组合条件），16 字节压缩头 PASS，合法 RAW_V2 控制 PASS。此普通构建没有 ASan，不能将一次 PASS 当作越界安全证明，也不能从组合退出码区分错误返回与内容变化。 | 在任何类型转换/字段读取前验证对应头长和 payload 长度；保留隔离回归，并在 sanitizer/debug STL 下复核。 |
| BUG-P10-002 | **当前测试误报；旧压缩格式边界待确认。** 现有有效 `BLOCK_VERSION_CMP` fixture 使用 `BlockHeaderV2 + _size + 压缩旧结构 payload`；reader 的 `day_old_struct_versions_are_upgraded` PASS。P10 用例却用 12 字节 `BlockHeader + raw 新结构` 标成 CMP，所以 false 结果无证明力；未找到真实 12 字节 CMP 样本。 | 以有效 V2 CMP 头和压缩 `WTSBarStructOld` 直接验证 writer；如需兼容无 `_size` 格式，先补协议/真实文件证据再增加分支。 |
| BUG-P10-003 | **源码行为确认、缺陷性待格式契约。** 原测试把接受错误 magic 视为通过，现改为禁用的契约待定用例；必须拒绝坏 magic 的规范未从接口确认。`_type` 检查需在调用方按预期类型执行。 | 规范要求时在解析版本前校验 magic，并由调用方校验类型；测试断言规范。 |
| BUG-P10-004 | **输入 header-only 文件时静默丢新 bar，属数据持久化缺陷。** 修正后的空 payload 解码控制 PASS，追加持久化契约 FAIL；真实掉电残骸未复现。 | 空 payload 作为空历史追加新 bar；无法安全恢复时返回失败，不得静默成功。 |
| BUG-P10-005 | **主 writer 队列同步缺陷候选，源码可证、动态未复现；标题中的 `_async_task` 未初始化是误植。** `_async_proc` 在 `init()` 赋值；`_tasks` 与 `_proc_que` 均有锁外判空/无谓词等待，`_proc_que` 还未在 producer 与 consumer 间共用锁。 | 以同一锁保护队列并用谓词等待；release 的唤醒/join 生命周期需明确。 |

**命令证据：** `build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_filter='P10ProcBlockTest.*'`：11 项 PASS，DISABLED 短输入用例未运行；其中 CMP false 用例只验证了错误构造的 12 字节头 + raw payload 被拒绝。`build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_filter='P10DayDsbTest.EmptyHistoryFileWithHeaderOnlyDecodesToEmptyPayload'`：1 项 PASS，现状为返回 true、文件仍 12 字节。`build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_filter='Q03RdmKlineTest.day_old_struct_versions_are_upgraded'`：1 项 PASS，证明 reader 测试夹具接受 V2 CMP 头加旧结构压缩载荷；此结果不是 writer 的直接动态测试。线程相关用例未运行。
