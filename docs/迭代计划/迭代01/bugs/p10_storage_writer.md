# p10_storage_writer 疑似缺陷（状态：待审核，未改任何生产代码）

对象文件：`src/WtDataStorage/WtDataWriter.cpp`（2701 行）。行号来自本轮 Read 实测。

## BUG-P10-001（Major，崩溃类）`proc_block_data` 对输入长度零校验
- 位置：`WtDataWriter.cpp:1818-1844`
- 现象：`BlockHeader* header = (BlockHeader*)content.data();` 后立刻 `header->is_compressed()`，
  对 `content` 是否为空、是否至少 `sizeof(BlockHeader)`/`sizeof(BlockHeaderV2)` 无任何检查。
  空 `std::string` 的 `data()` 在 libc++ 下可为 nullptr → 解引用段错误；
  短于 20 字节但 `_version == BLOCK_VERSION_CMP_V2` 时读 `blkV2->_size` 越界，
  并以声明的 `_size`（攻击者可控/文件损坏可控）调 `WTSCmpHelper::uncompress_data` → 越界读。
- 与 p05 同源：读取侧同样缺长度校验，见 `bugs/p05_datastorage2.md` BUG-P05-006。
- 处置：触发输入整类收进 `src/TestUnits/coverage/p10_storage_writer/p10_proc_block_data_test.cpp`
  的 `P10ProcBlockTest.DISABLED_CrashClassInputs`，启用用例零残留。

## BUG-P10-002（Major）老版压缩块永远无法解码
- 位置：`WtDataWriter.cpp:1822-1841`
- 现象：`BLOCK_VERSION_CMP`（0x02）同时命中 `is_compressed()` 与 `is_old_version()`，
  但压缩分支一律按 `BlockHeaderV2`（20 字节，含 `_size`）做
  `content.size() != sizeof(BlockHeaderV2) + blkV2->_size` 校验；
  老格式头部只有 `BlockHeader`（12 字节，无 `_size`）→ 校验恒失败、直接 `return false`，
  老的压缩历史文件既解不开也不报错，上层只看到「读不到数据」。
- 实测证据：启用用例 `P10ProcBlockTest.OldCompressedFlagGoesThroughV2HeaderCheck`
  断言返回 false 且 content 未被改写。
- 理想行为（修复后可启用）：老压缩块应按 12 字节头 + 剩余全部字节解压，并升版为 RAW_V2。

## BUG-P10-003（Minor）`proc_block_data` 不校验块标志 `_blk_flag`
- 位置：`WtDataWriter.cpp:1818-1831`（全函数无 `BLK_FLAG` 比对）
- 现象：把 `_blk_flag` 写成 `"WRONGFLG"` 或整块类型写成别的 `BlockType`，函数照样按 `_version` 位
  透传/去头返回 true。文件被截断或错位时不会被发现，坏数据会静默进入后续 `memcpy` 装配。
- 实测证据：启用用例 `P10ProcBlockTest.BlockFlagNotValidated`（按现状断言）。

## BUG-P10-004（Major，待审核）只有头部的历史 `.dsb` 会静默丢弃新 bar
- 位置：`WtDataWriter.cpp:1951-2006`
- 现象：`dump_day_data` 走「文件已存在」分支时，`barcnt = content.size()/sizeof(WTSBarStruct)`；
  若文件只有 12 字节头部（前次写入被中断/截断的典型残骸），`barcnt == 0` 使
  `if (barcnt != 0)` 的追加分支整体跳过，随后仍 `truncate_file(0)` + 只重写头部 → 本次 `newBar` 丢失，
  而函数返回 `true`（`bNeedWrite` 被标 `[[maybe_unused]]`，从未参与判断，`:1936`）。
- 后果：日线断点后无法自愈，需人工删文件；调用方（`dump_bars_to_file`）看不到失败。
- 实测证据：启用用例 `P10DayDsbTest.EmptyHistoryFileWithHeaderOnlyDecodesToEmptyPayload`
  按现状断言「返回 true 但文件仍是 12 字节」；修复后该断言应改成 `12 + sizeof(WTSBarStruct)`。

## BUG-P10-005（Major，沿用 p05 编号）`_async_task` 未初始化 + 无谓词 `condvar.wait` 丢唤醒
- 同源条目：`bugs/p05_datastorage2.md` BUG-P05-005（`WtDataWriterAD`）。
- `WtDataWriter` 侧同型风险点：`pushTask`（`WtDataWriter.cpp:578-620`）与 `proc_loop`（`:2216`）
  在 `_async_proc = true` 时依赖后台线程 + `_task_cond.wait()`；
  `check_loop`（`:1616`）内含 `sleep_for(10s)`（`:1621`）与清理线程 `sleep_for(300s)`（`:2385`）。
  本轮**未构造任何进入线程分支的用例**（见 manifest 跳过项 1/2），因此未复现，仅登记避免重复计数。
