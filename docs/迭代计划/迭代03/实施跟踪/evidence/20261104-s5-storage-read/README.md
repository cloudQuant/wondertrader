# s5 storage-read：历史存储读端、LMDB 键与格式、切片（2026-11-04）

macOS arm64，HEAD `ccaad010`（工作区含大量他人未提交改动）。本目录只记录本包定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实数据、跨进程生产部署或最终整合树。未修改任何跟踪文件。并行的 date-session 包在 17:55 与 18:12 改写了 `WtDataReaderAD.cpp`、`WtRdmDtReaderAD.cpp`（日期映射调用点，并把两文件行尾改为 LF）及 `WtDataReader.cpp`、`WtRdmDtReader.cpp`；`patches/*.diff` 以 `--strip-trailing-cr` 生成，其中 `DateMappingClient`/`ad_reader_trading_date` 相关行不是本包改动。

## 1. 修改文件（改前副本在 `before/`，哈希见 `hashes/`）

| 文件 | 改前 sha256 | 改后 sha256 | 内容 |
|---|---|---|---|
| `src/WtDataStorageAD/LMDBKeys.h` | `78faa60e7103` | `3d7127b66af6` | L04-004：构造器只拒绝 NULL/超长/未终止（有界扫描、`invalid_argument`），恢复 HEAD 对空字段的接受；入口校验 `lmdbKeyIdentityValid` 仍拒空；`static_assert` 固定 52/56 字节键布局 |
| `src/WtDataStorageAD/WtDataReaderAD.cpp` | `2e42c70320ba` | `ecce89cb3e2b` | L04：有界日志（`format_to_n`+截断标记）、入口身份拒绝带 ERROR；R100/R32：分钟 cutoff 与 minute-end 64 位计算，超出 32 位键范围明确拒绝且不发 `on_all_bar_updated`；R122：增量 tick 暂存上限=环形缓存容量，前向 K 线两阶段（快照内全量校验，≤4096 条直接应用，超出则分块重读、事务关闭后回调），测试钩子 |
| `src/WtDataStorageAD/WtDataReaderAD.h` | `12d57e683197` | `26aff1e22e8b` | 测试钩子声明与 friend |
| `src/WtDataStorageAD/WtRdmDtReaderAD.cpp` | `1e1e24d4f790` | `356bed19e744` | L04 同上；R100：`readKlineSliceByRange` 分钟端点 64 位、1990 前钳 0；R21-BUG-07：记录扫描快照的 `me_last_txnid`，无新提交时复用缓存，有提交仍全量重扫 |
| `src/WtDataStorageAD/WtRdmDtReaderAD.h` | `c4c9b1a2f105` | `12609305d06a` | BarsList 增加快照事务号字段（模块内部结构） |
| `src/WtDataStorageAD/WtBtDtReaderAD.cpp` | `46e41ba227ff` | `8939131b3ee7` | L04：有界日志、入口身份拒绝带 ERROR |
| `src/TestUnits/coverage/p05_datastorage2/s5_reader_ad_bounds_test.cpp` | 新增 | `ec5b6b11a3ad` | 15 例（L04/R100/R32/R122） |
| `src/TestUnits/coverage/p05_datastorage2/s5_rdm_ad_snapshot_test.cpp` | 新增 | `4c896a510c6c` | 4 例（R21/R100/L04） |
| `src/TestUnits/coverage/p05_datastorage2/r21_rdmbars_kline_test.cpp` | `before/…/r21_rdmbars_kline_test.cpp.before` | `4291b5c9c4e2` | 旧断言 `Reading back==2` 改为 1（理由见 §4 R21） |
| `src/TestUnits/coverage/p05_datastorage2/p05_rdmdtreader_ad_slices_test.cpp` | `1a2f6611839a` | `8bea1f9f186b` | `KlineRangeKeepsExistingCacheWhenCheckedQueryFails` setup 增加一次同内容提交以强制重扫，断言不变（理由见 §3.4） |
| `src/TestUnits/fixtures/l04_lmdb_key_bounds/key_bounds.cpp` | `before/…/key_bounds.cpp.before` | `a0b4c1a5ba1e` | 空字段由“构造器拒绝”改为“构造器接受、入口拒绝”（理由见 §4 L04） |
| `src/TestUnits/fixtures/i03_storage_read_20261104s5/{CMakeLists.txt,run.sh}` | 新增 | `a91ca353d720` / `ea028a7adf98` | 红绿夹具：`s5_ad_read`、`s5_ad_lane`（p05 整条 lane）、`s5_storage_z`（Z 类复跑） |

未修改：写端（`WtDataWriter*.cpp`，属 dt-writer 包）、`WtDataReader.cpp`/`WtRdmDtReader.cpp`（WtDataStorage，本包无需改动；被 date-session 包并行修改）、`WtDtMgr.*`、`LMDBBarKeyV2.hpp`/`LMDBBarMigration.hpp`、`DataDefine*.h`、`RTBlockPublish.hpp`、WtDtServo（R123-003 的产品文件不在本包所有权内）。

## 2. 构建

- 普通库：`/tmp/wt-i03-s5-storage-read-core`（Homebrew LLVM 22.1.8，`-include cstdlib`，`WT_WARNINGS_AS_ERRORS=OFF` 仅为依赖库本地变通），只构建 `WTSUtils WTSTools`。
- 夹具（ASan+UBSan，含 alignment）：`/tmp/wt-i03-s5-storage-read-green-asan`（工作区）、`/tmp/wt-i03-s5-storage-read-red-asan`（红：`/tmp/wt-i03-s5-storage-read-redsrc/src` = 工作区相关目录 rsync `--no-links`，`find -type l` 为 0，6 个读端/键文件替换为 `before/` 副本，`WT_S5_BASELINE=ON` 排除依赖新钩子/新诊断的 4 例）。
- 默认 `-Werror`（Apple clang）：本包 3 个产品 TU 与 3 个测试 TU 以覆盖率构建同款参数 `-fsyntax-only -Werror` 全部 exit 0；in-tree `CoverageTests_adstorage` 见 §3.4。
- 运行：每个二进制在 `/tmp/wt-i03-s5-storage-read-run/<label>` 独立 cwd，`perl -e 'alarm N; exec @ARGV'` 硬超时。

## 3. 结果

### 3.1 红绿（`runs/s5-green-asan`、`runs/s5-red-asan`）

| | 绿（工作区） | 红（改前读端/键文件） |
|---|---|---|
| `s5_ad_read`（新 19 例 + r21 旧 lane 10 例，含 1 例既有 DISABLED） | 29/29，exit 0；连续重复 ×25 全过 | 25 例中 7 例失败，exit 1 |

红控失败项与含义：
- `S5LmdbKeyBounds.EmptyFieldsFitTheKeyButAreNotAStorageIdentity`：改前构造器拒空字段（同时使 l04 lane 旧用例失败，见 3.2）。
- `S5ReaderADTest.LongConfiguredPathLogIsBoundedToTheMessageBuffer`：改前 `fmt::format_to` 写穿 512 字节 TLS 日志缓冲（ASan 不覆盖 thread_local，故以长度断言观测）。
- `S5ReaderADTest.MinuteCutoffBeyondUint32KeyIsRefusedNotWrapped`：改前 2033 cutoff 回绕到约 6.06e6，**实际返回了无关的早期 bar（time=1000）**。
- `S5ReaderADTest.MinuteEndBeyondUint32KeyDoesNotWrapOrSignalAllUpdated`：改前回绕后仍调用 `on_all_bar_updated`。
- `R21ReaderADTest.S5SameWindowWithoutCommitReusesSnapshot` 与旧 `RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated`（新断言）：改前无提交也每次全量重扫。
- `R21ReaderADTest.S5MinuteRangeEndingIn2033DoesNotWrap`：改前右端点回绕，2032 年 bar 返回 NULL。

无运行红控、仅静态分析的项：R122 有界暂存（改前用 `get_range_checked` 把整段复制为 `std::string` 数组再回调，内存随跨度线性增长；新钩子在改前代码中不存在，无法插桩对照）；损坏不部分提交、只读目录两例在改前也通过（行为回归控制，非红绿）。

### 3.2 l04 lane 键用例（`runs/l04-keys`）

`l04_lmdb_key_layout_test.cpp` 单独编译（与主目标同源文件）：改前工作区头 17 例 1 失败（`ExchgPrefixDoesNotCollideAcrossContracts` 用空字段构造键被新抛异常打断），HEAD 头 17/17，本包头 17/17；`l04_lmdb_key_bounds/key_bounds.cpp`（ASan/UBSan）exit 0。

### 3.3 p05 整条 lane（夹具 ASan/UBSan，`runs/ad-lane-asan-*`）

- 含 alignment 检查：在既有写端用例 `R4WriterADTest.ReloadExistingCacheRebuildsIndex` 的测试夹具 `make_tick_item`（打包 `TickCacheItem`）处 UBSan 中止（I03-NEW-007 范围，非本包代码），见 `ad-lane-asan-align/out.log`。
- 排除 alignment（`WT_SAN_ALIGNMENT=OFF`，仅此一项）：整条 lane 首先在既有 `R122WriterADErrorsTest.R122WriterADUpdateTickCacheUninitialized`（写端，`checkedTickSessionDate` SEGV，改前两侧均 abort 的既有项）崩溃；排除该例后 289 例：273 通过、0 失败、16 DISABLED；进程 exit 1 仅因 LSan 报告既有夹具泄漏（`r162ad::AdFixture::make_reader/make_writer` 等，R162/R172/R182/R203 用例未释放对象），泄漏栈中无本包函数或本包用例。
- 本包新用例在含 alignment 的 `s5_ad_read`（LSan 开）中 29/29、×25 重复全过。

### 3.4 in-tree `CoverageTests_adstorage`（默认 -Werror 覆盖率构建，`/tmp/wt-i03-s5-storage-read-cov`）

第一次整条 lane 运行（18:57 链接）：290 例，2 失败、16 DISABLED。失败一为既有写端 `R122WriterADUpdateTickCacheUninitialized`；一为 `P05RdmReaderADTest.KlineRangeKeepsExistingCacheWhenCheckedQueryFails`——该用例依赖“同窗口第二次读取必然重扫”来注入 checked range 失败，本包的快照复用使第二次读取不再扫描。处理：仅在其 setup 中写入一次相同内容的提交以强制重扫，断言全部保留（`before/…/p05_rdmdtreader_ad_slices_test.cpp.before`）。复跑（`runs/intree-adstorage`）：290 例，273 通过、1 失败（仅既有写端 `R122WriterADUpdateTickCacheUninitialized`）、16 DISABLED，exit 1。in-tree 二进制含本包最终读端源码（18:12 date-session 修改之后编译），哈希见 `hashes/binaries-sha256.txt`。

## 4. 条目结论与理由

- **BUG-L04-004（E）**：键构造器有界复制 + `invalid_argument` 明确错误，磁盘布局以 `static_assert` 固定；三个 AD 读端所有入口在建目录/开库/建缓存前拒绝并输出 ERROR（Data 3 入口、Rdm 5 入口、Bt 2 入口均有用例，且断言无目录产生）。改前工作区头把空字段也改成抛异常，导致 l04 lane 旧用例失败；按 HEAD 行为恢复“构造器接受空字段、入口拒绝空身份”，同步修改本条自有夹具 `l04_lmdb_key_bounds/key_bounds.cpp` 中与旧 lane 用例矛盾的两处空串断言（无裁定卡支持“构造器拒空”，以较早的 lane 合同为准，需主线程确认）。写端入口校验（`writeTick` 准入）已由卡13 子任务实现并有正控；写端未做的部分（准入拒绝不打日志、`pipe_writer_log` 无界格式化、异步“接收 vs 持久化成功”语义）属 dt-writer 包。建议：**待验证**（读端与键本地通过；待 dt-writer 包确认写端子项后整项可本地验证通过）。
- **BUG-R100-003 / BUG-R32-003（D）**：读端 2033 起不再回绕（`readKlineSlice` 分钟 cutoff、`onMinuteEnd` minute-end 64 位计算，超出 V1 32 位键即 ERROR+拒绝，不发 `on_all_bar_updated`；`readKlineSliceByRange` 端点 64 位），红控证明改前会返回无关早期 bar；写端拒写由卡13 子任务完成。`LMDBBarKeyV2.hpp`/`LMDBBarMigration.hpp` 仍未接入：V2 存放位置、切换/回滚、证据来源与部署门禁无合同（决策卡 S5-A）。建议：**阻塞**；解除条件：用户裁定 S5-A（若裁定“维持 V1 + 2032 上限 fail-closed”，可凭本证据转本地验证通过）。
- **BUG-P05-010（X）**：未改动。保持**阻塞**；解除条件不变（有来源的历史 LMDB 键样本与版本说明）。
- **BUG-R122-002（D）**：可确定部分完成：增量 tick 暂存上限=缓存容量（单快照全量校验）；前向 K 线两阶段——阶段一在单快照内流式校验整段，≤4096 条直接从同一快照应用，否则阶段二按块重读，每块独立校验、事务关闭后回调；损坏数据（阶段一可见）整段拒绝、零回调零提交、修复后重试每根恰好一次；阶段一与二之间的并发改写只会停在已应用位置，重试不重复回调（用例覆盖）；只读根目录缺库/只读交易所目录缺 data.mdb 均 NULL+ERROR、无异常、无部分缓存，恢复权限后同一 reader 重试成功（与既有“目录创建失败即使有 RT bar 也返回 NULL”合同一致）。HOT/custom 跨换约：首载用品种键、`readKlineSlice` 增量用物理合约、`onMinuteEnd` 增量用品种键，混源未闭合（决策卡 S5-B）；分钟 uint32 上界随 S5-A。建议：**阻塞**；解除条件：S5-B（及 S5-A）裁定。有界内存无运行红控（静态分析）。
- **R21-BUG-07（D）**：只实施不改变可见性的部分（无新提交复用快照，有提交仍全量重扫，修订即时可见）；未恢复迭代02 撤回的游标候选。旧 `RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated` 的 `Reading back==2` 是对本缺陷的现状刻画（注释“本应只补…实测全部重读”），与本条原验收条件“同窗口复读不扫全量”直接冲突，改为 1 并保留 `.before`——该改写不是依据裁定卡，需主线程确认。建议：**阻塞**；解除条件：用户裁定 S5-C（有提交时的增量游标与更早 bar 修订可见性）。
- **BUG-R100-004（D）**：未改产品、未改 oracle。卡13 协议已使“已发布槽完整”可证明，剩余为可见性合同，且两读端现不一致（决策卡 S5-D）。当前树 `R100RdmCrossDayTest.R100RdmRtHftCrossDateBackoff` 仍失败（`runs/rdm-regress-1812`，该次快照另有大量与本条无关的 RT 失败，见 §7-4）。建议：**阻塞**；解除条件：用户裁定 S5-D 后实施并补跨进程尾槽用例。
- **BUG-R132-001（D+实现）**：未改产品。CMP_V2 资源预算待裁定（S5-E）。未对齐 UB 以运行证据复现：开启 UBSan alignment 运行 `R162ReaderEdgeTest.R162RdmReaderHisTransBlocksWithMixedActionDates` 在 `lower_bound` 处因 `WTSOrdQueStruct` 地址未按 8 字节对齐中止（`runs/z-asan/R162…/out.log`），关闭 alignment 后同例通过（`runs/z-plain`）。仍用打包缓冲直接取记录指针的位置：`WtRdmDtReader.cpp` tick 3 处（481/650/2971 行附近）、委托队列/明细/成交 3 处（968/1182/1395 行附近）、`WtDataReader.cpp` tick 1 处（705 行附近）；`WtDataReader` 的队列/明细/成交已用 `copy_his_items_aligned`。这两个文件正被 date-session 包并行修改，本包未动。建议：**实施中**（对齐修复为实现项，预算待 S5-E）。
- **BUG-R123-003**：产品文件在 WtDtServo（不在本包所有权内）。静态核对：`WtDtRunner` 析构先 `shutdown()` 按卡9 请求停止并排空全部 parser（失败则 owner 保留到进程退出、回调门关闭），成员按声明逆序析构使 `_parsers` 先于 `_data_mgr` 销毁；`WtDataManager` 析构先释放自有缓存/实时 map，再经 remover 销毁 reader；reader 模块成功加载后不卸载（与卡9a“无法证明排空则常驻”及卡11“持有切片期间不得卸载 owner 模块”一致）。运行证据：卡11 夹具 `i03_slice_servo`（WtDtServo 数据管理器清缓存/增量/析构 remover 恰好一次/部分初始化不调 remover/清实时 map）在当前树 ASan+UBSan(除 alignment)+LSan 5/5 通过（`runs/r123-servo`）。建议：**待验证**（需 WtDtServo 负责包补“runner 停止后析构无 parser 回调访问”的计数 fake 用例；DLL 不卸载为设计选择，写入接口文档）。
- **BUG-I02-NEW-095**：在当前树（含卡11 改造后的 `HisDataReplayer.cpp` sha256 `9a81cdd29200…`、R180/R210 测试 TU 与迭代02 交接记录一致 `d9b04e37…`/`0f8fe1ae…`）以默认 `-Werror` 覆盖率构建 `CoverageTests_btcore`（exit 0；z15 记录的 gtest sign-compare 构建失败在本次未出现），五个原验收单例各自独立进程 1/1 通过：`R180His_LoaderM5AutoTransThenDsbReuse`、`R180His_CorruptAutoTransDsbRebuiltFromLoader`、`R180His_CheckUnbarsMinute5UsesUnbarsCache`、`R210His_BarReplaySkipsEmptyL2Lists`、`R210His_UncompressedV2BarBlockHeaderStripped`；关联组 `R180LoaderFixture.*:R180ReplayFixture.*:R210Fixture.*` 16 例中 15 执行、14 通过、1 失败（NEW-096）、1 DISABLED（`runs/btcore/`）。本项五例与原验收一致；剩余仅 NEW-096（独立条目）与整体回归。建议：**本地验证通过**（不含 NEW-096、其他平台、整体回归；非 sanitizer 构建）。

## 5. 决策卡

见 `决策卡草稿.md`：S5-A（V2 分钟键接入）、S5-B（HOT/custom 跨换约）、S5-C（R21 增量游标与修订可见性）、S5-D（R100-004 末槽可见性）、S5-E（CMP_V2 解压预算）。

## 6. Z 类复核（卡 15）

| 条目 | 复核结果 | 建议处置 |
|---|---|---|
| NEW-032 / NEW-082（同一用例 `Q03RdmKlineTest.day_tail_fragment_is_dropped`） | 当前树仍为产品整块拒绝（返回 NULL）、测试期望保留完整前缀，失败形态与迭代02 相同（`runs/z-asan/Q03…`）。仅 HOT 有 Q03-001 合同（坏块不得部分缓存）；普通合约日线无合同。整块拒绝符合“fail-closed 优先”，未发现越界或缓存污染缺陷。 | 无明确产品缺陷 → 按卡15 记“用户指示忽略”（保持阻塞，解除条件：用户确认普通日线尾碎片按整块拒绝或按前缀恢复）；若用户愿意一并确认“与 Q03-001 一致整块拒绝”，即可改写该旧断言并关闭两条。 |
| NEW-059 | `R162…HisTransBlocksWithMixedActionDates` 现通过，但：产品在提交 `e03cbe1d`（迭代02 修复）由“含端点”改为 `lower_bound` 右端排他，测试断言随后在检查点 `ccaad010` 由 `etime=stime` 改为 `stime+1`（注释“历史 HFT 区间右端排他”）；两处均无合同依据。现状：历史委托/明细/成交右端排他，同一 reader 的实时 HFT 右端含（`upper_bound`），K 线右端含，AD tick 区间右端排他——端点语义不一致。 | 未发现可直接认定的缺陷；按卡15 记“用户指示忽略”，但须同时报告“断言曾无合同改写”。如用户愿意，建议另出一张“区间端点统一语义”小卡。 |
| NEW-060 | `R193StorageReaderTest.R193LoaderUnknownPeriodUsesDayName` 在当前树单独运行 1/1 通过（`runs/z-asan/R193…`），不再复现迭代02 的空返回；但以 `KP_Tick` 调 `readKlineSlice` 是否合法、默认周期名称仍无公开合同。 | 现状通过不等于合同成立 → 按卡15 记“用户指示忽略”（不计通过），注明当前用例通过。 |
| NEW-096 | 静态核对 `HisDataReplayer`：`get_kline_slice` 对每个被查询的序列置 `_notify_close=true`（4482 行附近），`onMinuteEnd` 把所有 `_notify_close` 序列放入 `replayBars` 并逐一 `mark()` 把 `_untouch_days` 清零（4104 行附近）；`check_cache_days` 只在会话开始时 +1 并按 `>=cache_clear_days` 淘汰（7149 行起，字段注释“未用到的天数”）。A/B 被查询后每个交易日都随回放被标记为“已用”，阈值 2 永远达不到，阈值 1 则在会话开始、标记之前即被淘汰——与迭代02“阈值 1 通过、阈值 2 失败”的诊断一致。测试前提“A/B 在第二个交易日被淘汰”与产品“在用序列不淘汰”的语义矛盾。运行证据（`runs/btcore/new096_lldb/lldb.log`）：当前树单例仍失败（loader 调用 1≠2）；lldb 计数 `check_cache_days` 命中 5 次（5 个交易日）、`_BarsList::mark()` 命中 15 次（主/A/B 三序列 × 5 日），证实每日标记清零。 | 判定为测试期望问题而非产品缺陷，但改写旧断言无裁定卡依据：建议用户确认“被订阅并逐日回放的序列视为在用、不淘汰”，之后把该用例改为断言阈值 2 下不淘汰（loader 仍 1 次），并另加“未再订阅的序列在 N 个会话后淘汰”的正控；确认前保持阻塞。阈值 1 会在会话开始时淘汰在用序列（下一次查询重载），属边界行为，一并请用户确认是否接受。 |

## 7. 新发现（编号由主线程分配）

1. **读写端日志无界格式化**：`pipe_*_log` 以 `fmt::format_to` 写入 512 字节 thread_local 缓冲，消息含配置路径/调用方代码时可写穿（ASan 不覆盖 TLS；用例以超长路径证明改前写满 512）。本包已修三个 AD 读端；未修：`WtDataWriterAD.cpp:pipe_writer_log`（dt-writer 包）、`WtRdmDtReader.cpp`/`WtBtDtReader.cpp`（`fmtutil::format_to`/`fmt::format_to`，WtDataStorage）。`WtDataReader.cpp` 已有有界实现。
2. **缓存键 `fmtutil::format<64>` 以调用方 stdCode 拼接**：`CodeHelper::extractStdCode` 允许每段 <32 字节、最多 4 段，stdCode 可超过 64 字节，`format<64>` 无界写 thread_local 缓冲（AD 读端、WtDataReader、WtRdmDtReader 多处）。仅静态分析，未构造红控、未修。
3. **`WtRdmDtReader` RT 路径未采用卡13 读者可见性规则**：RT 委托队列/明细/成交以 `_size - 1` 固定排除末槽，RT tick/K 线却可经回退判断包含末槽；均以普通读取 `_size`，未 acquire、未按本进程映射钳制，与 `WtDataReader`（卡13 规则）不一致（关联 S5-D、R100-004）。仅静态分析。
4. **当前树 WtRdmDtReader 回归（疑为并行修改中间态）**：18:12 前后编译的快照上 `i03_slice_rdm_regress` 出现大量 RT 拼接/区间用例失败（如 `min1_splices_his_block_with_rt_block` 得 2 期望 4）及 `r23_rdm_count_slice_test.cpp:416` 空指针 UB 中止（exit 134）；卡11 证据中同一目标为 176/177。本包未改这两个文件，疑与 date-session 包对 `WtRdmDtReader.cpp`/`WtDataReader.cpp` 的进行中修改有关，建议该包完成后复跑（`runs/rdm-regress-1812`）。
5. **NEW-059 断言在检查点提交中被无合同改写**（见 §6）。

## 8. 限制

仅本机 macOS/APFS、本地 fake 与临时 LMDB；未覆盖 Windows/MSVC、Linux、真实历史库、跨进程长时间负载、断电。Apple clang ASan 二进制未使用（已知 main 前挂起），sanitizer 结论来自 Homebrew LLVM 22 夹具，依赖库未插桩。整体回归 NOT_RUN。

## 9. 第二轮（date-session 完成后，2026-11-04 晚）

改前副本 `before-r2/`（每个文件改前重新读取并复制，保留 date-session 的 DateMapping 改动），哈希 `hashes/before-r2-sha256.txt` → `hashes/after-r2-sha256.txt`，差异 `patches-r2/`（`--strip-trailing-cr`）。红控源码根 `/tmp/wt-i03-s5-storage-read-redsrc2/src`（相关目录 rsync `--no-links`，`find -type l`=0，产品文件与 `r4_ad_fixture.h` 替换为 `before-r2/` 副本，哈希 `hashes/red-r2-snapshot-sha256.txt`）。所有运行器改用 `exec @ARGV or exit 127` 并以 XML 存在判定，避免缺二进制误报通过（第一次红跑因红构建失败、二进制缺失而“全过”，已作废重跑）。

| 文件 | 改前 | 改后 | 内容 |
|---|---|---|---|
| `src/WtDataStorage/WtRdmDtReader.cpp` | `7cd12942a414` | `e371455a3c04` | R132：6 处历史块记录改由 `align_his_items` 迁入对齐 `_items` 后读取/出切片；NEW-052：7 个入口 + `cacheHisBarsFromFile` + `getAdjFactorByDate` 判空；NEW-038：有界日志 |
| `src/WtDataStorage/WtRdmDtReader.h` | `961f7ef89bcd` | `1905b4b217c2` | 四类历史块 pair 增加 `std::vector<T> _items` |
| `src/WtDataStorage/WtDataReader.cpp` | `bfd7e7d5f48a` | `884514ba32c6` | R132：历史 tick 改用对齐副本；NEW-052：4 个入口 + `cacheHisBarsFromFile` + `getAdjFactorByDate` 判空；NEW-039：5 处 `key[64]` 改 `std::string` |
| `src/WtDataStorage/WtDataReader.h` | `c19bf8238782` | `8802ce40fe5a` | `HisTBlockPair` 增加 `_items` |
| `src/WtDataStorage/WtBtDtReader.cpp` | `1d63b0cfea05` | `8d63d67a90ee` | NEW-038：有界日志 |
| `src/WtDataStorageAD/WtDataReaderAD.cpp` | `ecce89cb3e2b` | `aca7c7edab26` | NEW-052：两个入口判空先于映射；NEW-039：2 处 `format<64>` 改 `fmt::format` |
| `src/WtDataStorageAD/WtRdmDtReaderAD.cpp` | `356bed19e744` | `99d3c0404aef` | NEW-051：日期映射改用自然时间；NEW-052：判空（含 session）；NEW-039：2 处 |
| `src/TestUnits/coverage/p05_datastorage2/r4_ad_fixture.h` | `4806f2fecb93` | `bae0ce82b129` | NEW-049：`make_tick_item` 字节拼装、不默认构造打包项；新增对齐视图 `R4TickItemView` 读取重载 |
| `src/TestUnits/coverage/p05_datastorage2/r4_writer_ad_cache_test.cpp` | `66a93131fa90` | `faa5519953ee` | NEW-049：两处 `TickCacheItem` 栈对象改 `R4TickItemView`，断言不变 |
| `src/TestUnits/coverage/q03_storage3/s5_storage_reader_guards_test.cpp` | 新增 | `145b37ac6ccf` | R132 文件读取逐字段/对齐断言、NEW-052 两读端、NEW-039（DISABLED，见下） |
| `src/TestUnits/coverage/p05_datastorage2/s5_rdm_ad_snapshot_test.cpp` | 追加 | `6b63f887fb5d` | NEW-051 夜盘 +300 偏移、NEW-052 Rdm AD |
| `src/TestUnits/coverage/p05_datastorage2/s5_reader_ad_bounds_test.cpp` | 追加 | `28b5ce14964d` | NEW-052 Data AD |
| 夹具 `CMakeLists.txt` | — | `707b8c89683c` | 新增 `s5_storage_align`（alignment 开）与 `WT_SAN_ALIGNMENT` 选项 |

### 9.1 结果

- **BUG-R132-001 未对齐 UB（`runs/r2/storage-align-percase`，逐例独立进程，ASan+UBSan 含 alignment）**：44 例（新增 4 + R162/R112/q03_rdm_tick_file/R142/R132/R100 历史文件相关既有用例；排除测试代码自身未对齐的 `Q03RdmTickTest.his_tick_old_struct_versions_are_upgraded`，见 §9.3）。红（改前读端）16 例中止，其中 14 例为产品历史块 `reference binding to misaligned address` / `member access within misaligned address`（含 R162、R112、Q03RdmTick 7 例、R132、R100 两例、新增两例），2 例为 NEW-052 空指针；绿 44/44。整组一次运行同样 44 例、0 失败、6 DISABLED、无 sanitizer 报告（`storage-align-full`）。CMP_V2 解压预算仍待 S5-E。**建议 R132-001 → 待验证**（未对齐子项本地验证通过；预算裁定后实施）。
- **I03-NEW-052**：WtRdmDtReader/WtDataReader 红为 `member call on null pointer` 中止、绿通过（上表含）；WtRdmDtReaderAD `readTickSliceByRange` 红 SEGV、绿通过；WtDataReaderAD 两入口改前因日期映射先报 `MissingCommodity` 不崩溃，改后在映射前以 “not found” 拒绝（`runs/r2/ad-percase`）。
- **I03-NEW-051**：自建 +300 分钟夜盘替身（21:00–02:30，22:00 tick 以（20230104，偏移时间）建键，与 `WtDataWriterAD::commitTickToDb` 一致）：红（改前）区间 21:30–22:30 读不到该 tick（Failure），绿 1/1。
- **I03-NEW-049**：改前整条 p05 lane（alignment 开）在 `R4WriterADTest.ReloadExistingCacheRebuildsIndex` 中止（`runs/ad-lane-asan-align/out.log`）；修 `make_tick_item` 后又暴露同类 `TickCacheAtCapacityResizesFileAndKeepsOldItems`/`ProcFlagOneOnNewTradingDateRebaselinesCache` 两处栈上 `TickCacheItem`，一并改为对齐视图。修后整条 lane（alignment 开、排除既有写端崩溃用例）297 例 0 个 UBSan/ASan 报告，1 例失败 `R4WriterADTest.ReleaseWithoutTaskThreadIsInert`（release 后 `writeTick` 返回 false），重复 3 次相同；该用例只用写端，失败来自 dt-writer 包 20:46–20:50 对 `WtDataWriterAD.*` 的修改（第一轮同 lane 0 失败），非本包改动（`runs/r2/ad-lane-align-after-new049`）。
- **I03-NEW-038**：`WtRdmDtReader`、`WtBtDtReader` 的 `pipe_*_log` 改为 `format_to_n` + 截断标记（与 AD 读端相同）；仅静态分析 + 编译/回归（thread_local 缓冲不受 ASan 插桩，无运行红控）。写端 `WtDataWriterAD`/`WtDataWriter` 未改（dt-writer）。
- **I03-NEW-039**：本包读端 9 处以调用方 stdCode 拼接的固定缓冲键改为 `std::string`（WtDataReader 5、WtDataReaderAD 2、WtRdmDtReaderAD 2；`{}.{}` 由已校验字段拼接的不超 63 字节，未改）。长代码红控**未能运行**：63 字节合法 stdCode 在键之前/之后都会进入 `WTSTickSlice::create`/`WTSKlineSlice::create`，后者把 stdCode 无界 `wt_strcpy` 进 32 字节 `_code`，绿构建中即破坏堆并中止（`runs/r2/longcode-slice-overflow.log`）；该用例暂 DISABLED 并另报新问题。其他模块 24 处位置清单见 `runs/r2/new039-other-modules.txt`（WtBtCore CtaMocker/SelMocker/HisDataReplayer、WtDtServo WtDataManager、WtExecMon WtSimpDataMgr、WtCore WtDtMgr/CtaStraBaseCtx/SelStraBaseCtx/WtHftEngine/WtCtaEngine/WtSelEngine/WtEngine）。
- **I03-NEW-041**：date-session 完成后重编 `i03_slice_rdm_regress`：184 例 175 通过、2 失败（NEW-032 `day_tail_fragment_is_dropped` 既有；`P09WindowTest.public_kline_slice_readers_keep_empty_and_nonempty_results` 为卡1 替身 `calcTradingDate(0)` 返回 0，NEW-053 负责包已在改该替身），`runs/r2/rdm-regress-after-date-session`；叠加本轮改动后 184 例 176 通过、仅 NEW-032 失败（`runs/r2/rdm-regress-after-r2`）。此前“大面积失败”确认为 date-session 修改中间态，已不复现。**建议 NEW-041 → 关闭/本地验证通过**。

### 9.2 默认 -Werror

本轮 7 个产品 TU 与 5 个测试 TU（含 `r4_ad_fixture.h` 使用方）以覆盖率构建参数 Apple clang `-fsyntax-only -Werror` 全部 exit 0；in-tree `CoverageTests_adstorage` 见 §9.4。

### 9.3 本轮新发现

1. `WTSKlineSlice::create`/`WTSTickSlice::create`（`src/Includes/WTSDataDef.hpp:547/1817`）以 `wt_strcpy` 把调用方 stdCode 无界复制进 `_code[MAX_INSTRUMENT_LENGTH]`；`extractStdCode` 接受的合法 stdCode 可达 63 字节，读端出切片即堆越界（复现：`s5_storage_reader_guards_test.cpp` 的 DISABLED 用例，日志 `runs/r2/longcode-slice-overflow.log`）。不在本包所有权内。
2. `q03_storage3_fixture.h:650 expect_tick_upgraded` 以引用绑定打包 `WTSTickStructOld` 字段，UBSan alignment 下测试代码自身中止（与 NEW-049 同类，q03 lane 测试代码）。
3. `R4WriterADTest.ReleaseWithoutTaskThreadIsInert` 在当前写端（20:50 修改后）失败，归 dt-writer。

### 9.4 in-tree `CoverageTests_adstorage`（默认 -Werror，Apple clang 覆盖率构建）

本轮源码重编 exit 0；整条 p05 lane 298 例：281 通过、1 失败（`R4WriterADTest.ReleaseWithoutTaskThreadIsInert`，写端，见 §9.1 NEW-049 条）、16 DISABLED（`runs/r2/intree-adstorage`）。原既有写端崩溃用例 `R122WriterADUpdateTickCacheUninitialized` 在当前写端已通过。主目标（含 q03 新用例）未在 in-tree 重编，q03 新用例以夹具 `s5_storage_align` 运行并以 Apple clang `-Werror -fsyntax-only` 校验。

### 9.5 文件删除说明

本包只删除过自己创建的路径：`/tmp/wt-i03-s5-storage-read-run/` 下各次运行子目录（重跑前清空）、`/tmp/wt-i03-s5-storage-read-redsrc2`（重建前清空）、证据目录内误建的 `before-r2/src/TestUnits/coverage/p09_storage_readers`（该文件由 NEW-053 负责包修改，本包未改）以及证据中拷入的 `*.profraw`/`.dSYM`。未删除任何他人目录。另注：本会话中本包的 `/tmp/wt-i03-s5-storage-read-probe/` 曾被外部删除（非本包操作），已重建。
