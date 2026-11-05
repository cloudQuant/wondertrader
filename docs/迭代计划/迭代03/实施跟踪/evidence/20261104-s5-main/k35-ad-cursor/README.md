# 决策 K35=C（R21-BUG-07）：AD 读端 K 线缓存的末 bar 游标 + 有界陈旧

## 产品（`src/WtDataStorageAD/WtRdmDtReaderAD.{h,cpp}`，CRLF 保持）
- `readKlineSliceByRange`：
  - 环境自缓存快照后无任何提交时不读 LMDB（R21-BUG-07 既有部分）。
  - 有提交时，只读取 `[游标, 末尾]`（游标为缓存末 bar 的 LMDB 键：日线为日期，分钟线为 `bar.time`，含游标本身，因此末 bar 的同键修订立即可见），与缓存中键小于游标的 bar 合并。合并整体替换缓冲区，已借出的切片保持原内容（卡11）。
  - 更早 bar 的同键修订在下一次全量校验时可见。全量校验条件：请求终点交易日与上次全量时不同、增量次数达到 `full_check_increments`（默认 64）、距上次全量达到 `full_check_seconds`（默认 60），或末 bar 键不能用 uint32 表示。两项上限可在读端配置中设置，0 表示不按该项触发。
- 全量扫描结果为空时删除缓存项并返回 NULL；改前保留旧缓存。
- 去掉每次读取对缓存向量的整份深拷贝（改前 `candidate = cacheIt->second`）。
- 去掉对 `etime` 的就地改写，以及从未推进的 `_last_bar_time` 判断。
- 日志：全量为 "Reading back …"，增量为 "Reading newer … from cursor …"。

## 测试
- 新增 `p05_datastorage2/s5_main_k35_cursor_test.cpp`，4 例：
  - 追加与末 bar 修订走游标，旧切片不变，全量 1 次、增量 1 次；
  - 次数上限后，更早修订可见；
  - 时间上限（1 秒）后，更早修订可见；
  - 分钟线从游标合并。
- 按裁定改写：
  - `s5_rdm_ad_snapshot_test.cpp` 原 `S5CommitAfterSnapshotKeepsFullRescanAndRevisionVisibility`（任何提交全量重扫、修订立即可见、全量 3 次）改为 `S5CommitAfterSnapshotReadsFromCursorAndBoundsRevisionStaleness`：更早修订先陈旧，请求终点换日后全量可见；全量 2 次、增量 1 次。
  - `p05_rdmdtreader_ad_slices_test.cpp` 的 `KlineRangeKeepsExistingCacheWhenReloadFindsMalformedValue` 损坏对象由中间 bar 改为游标所在末 bar；原意图“重载遇损坏保留缓存、修复后恢复”不变。
  - 改前副本见 `before/`。
- 红控：用改前读端源码（K31 之后、K35 之前）重编，K35 4 例与改写的 S5 例共 5 例失败（`k35-red.log/xml`）；恢复改后源码（哈希见 `sources.sha256`）后定向 15/15 通过（`k35-green*.log`）。adstorage 逐例 286 例中唯一失败为上述损坏用例，改写后通过。
- 未覆盖：跨进程并发写读压力、真实数据量下的开销、Windows/MSVC。
