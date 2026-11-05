# 迭代03 卡3：日界线、行情日归属、日线与冻结量（2026-11-04）

用户裁定（2026-11-04，全部按建议）：3a=A 保留 `2400` 收盘标签、`23:59:60` 作为非法输入拒绝；3b=C 可配置、默认 A；3c=C 新数据 `day.time=0`、旧缓存兼容读取并加版本标记；3d=C 保存 `frozen`，由 session-begin 按快照 `tdate` 条件清零。涉及 `BUG-R202-001`、`BUG-R105-001`、`R4-BUG-13`、`BUG-Q02-003`。

macOS arm64，Homebrew LLVM 22.1.8，HEAD `ccaad010`（工作区含大量他人未提交改动）。改动前快照：`rsync -a --no-links src/ /tmp/wt-i03-card3-20261104/baseline/src/`（07:55 CST，早于本次任何编辑）。`evidence/product.patch` 是相对该快照的本次产品改动（52 个 hunk，仅本卡内容），`evidence/hashes.sha256` 为改动后/改动前哈希。无符号链接（`find evidence -type l` 为 0）。

## 1. 设计

### 3a（BUG-R202-001）ticker 日界：`src/WtCore/TickerDayBoundary.h` + `WtCtaTicker/WtSelTicker/WtHftTicker.cpp`
- `on_tick` 先校验 `action_time`：`HH<24、MM<60、SS<60`，否则记 ERROR 并丢弃（不 trigger、不改 `_date/_time`）。`23:59:60` 即被拒绝。
- 行情交易日 > 引擎交易日且 ticker 已有分钟位置：判定为新交易日首 tick。若旧日最后一分钟尚未发布，先按旧日期闭合（标签沿用 `minuteToTime`，全天时段为 `2400`），再把 `_cur_pos/_last_emit_pos` 归零。
- `_cur_pos==0` 分支（启动首 tick 与新交易日首 tick）改为：先 `set_date_time`，交易日更新时 `set_trading_date`，再交给引擎（原实现丢弃该 tick）。
- 全天时段 `00:00` 同时满足 `isLastOfSection`（`2400%2400==0`），原 `minutes--` 对 0 下溢为 `UINT32_MAX` 再 `++` 回 0，导致新日 `00:00` 后位置停在 0、`00:01` 不闭合。改为仅 `minutes>0` 时回退。
- 首版（下称 pre-9d）未处理串行化/重入/停止；该版本快照在 `/tmp/wt-i03-card3-20261104/pre9d-root/WtCore/`（哈希见 `evidence/hashes.sha256` 的 `pre9d` 行），作为 9d 的红控。
- 新交易日**不**自动调用 `on_session_begin`：卡9 9d=A 与 R202 合同只规定串行化、重入排队和 stop 排空，没有要求运行中换日触发 session-begin；原实时引擎也从不在运行中换日时调用它（只在 `run()` 调用一次）。如需此行为须另行裁定（会影响策略 `on_session_begin` 次数、引擎 `_ready` 与风控日初化）。

### 卡9 9d=A / R202-001 ticker 合同（2026-11-03 卡9 确认；2026-11-04 用户明确 R202 同 9d=A）

`TickerDayBoundary.h` 新增 `wt_ticker::EventStrand` 与 `runAndDrain`，三个 ticker（CTA/SEL/HFT）统一：
- `on_tick` 先 `enterEvent`：调用线程正是当前 owner（即在本 ticker 的策略/引擎/数据回调中同步重入）时，tick `retain` 后进 FIFO 队列并立即返回，不就地执行；否则等待成为 owner。原 `on_tick` 主体改名 `handle_tick`，只在 owner 内执行。
- worker 每轮 `enterWorker` 成为 owner 后执行 `worker_pass`（原线程体逻辑，去掉 `_mtx`），返回前由 `runAndDrain` 按 FIFO 投递本轮回调内排队的 tick（投递时的再重入继续排在队尾），每个排队事件恰好投递一次，然后释放 owner。全部 ticker 状态只在 owner 内读写。
- `_date/_time` 改为 `std::atomic<uint32_t>`（`is_in_trading()` 可在非 owner 线程读），CTA/SEL 的 `_stopped` 改为 atomic，并新增 `_running`（替代对 `_thrd` 的无锁读）与 `_stop_mtx`；删除 `_mtx`。
- worker 休眠改为可被 stop 立即唤醒的条件等待（交易时段 10ms，休市 10s）。
- `stop()`：置停止标志并唤醒；worker 线程（回调内）调用只发请求、不自 join；非 owner 调用方最多等待 `STOP_DRAIN_TIMEOUT`（5s）让当前事件与队列排空（超时记 WARN），再 join（CTA/SEL 新增 `_stop_mtx` 防并发 join；HFT 保留原生命周期状态机，只在 join 前加排空等待）。owner 线程（如策略回调内）调用 stop 时不等待排空，worker 因停止请求不再取得 owner 而退出，队列在该 owner 回调返回后照常投递。CTA/SEL 析构调用 `stop()`。
- 回调抛异常时 `abandon()` 丢弃排队事件并释放 owner 后重抛（避免永久占用）。
- 其他线程（如第二个行情线程）调用 `on_tick` 会阻塞等待当前 owner 完成——与原实现在 `_mtx` 上等待的行为同类，但范围扩展到整笔 tick 处理。
- 头文件布局变化（成员类型/新增成员、删除 `_mtx`）：依赖 ticker 对象布局的 TU 必须重编；`#define private public` 的旧测试对 `_date/_time` 的赋值/比较仍可编译。

### 3b（BUG-R105-001）ActionDay 纠偏：`src/Share/ActionDayNormalizer.hpp` + ParserCTP/CTPMini/Femas
- 仅 `ActionDay == TradingDay` 时：行情 20–23 时（原逻辑不变）或 **00 时**（模式 A）进入纠偏；本地 03–08 时丢弃（原过滤语义，亦适用于 00 时）；否则取本地日期，行情 23 时/本地 00 时取本地前一天，行情 00 时/本地 23 时取本地次日。自然日推进，不跳周末节假日（周五夜盘 00:30 → 周六）。
- 配置项 `midnight_actionday`：缺省/未知值 = A；`legacy`/`B`/`keep` = 旧行为（00 时不纠偏）。头文件新增私有成员 `m_iMidnightMode`（对象布局变化，模块内部类）。
- 只有进入纠偏分支的时段才读本地时钟（日盘不增加系统调用）。`WT_ACTDAY_TEST_CLOCK` 仅测试构建注入时钟。
- CTP 的 `localtime: true` 路径与 CTPMini 的 `ActionDay` 为空回退交易日保持原样。

### 3c（R4-BUG-13）AD 日线：`src/WtDataStorageAD/DayBarContractAD.h` + Writer/Reader/RdmReader/BtReader
- 新日线 `time=0`、`date`=交易日（与通用 WtDataWriter 一致）；跨日落库的 LMDB day 记录同为 `time=0`。分钟线不变。
- 版本标记：`cache_d1.dmb` 头 `_version` 1=旧格式（time 可能为交易日），2=全部 `time=0`。新建日线缓存写 2；writer 打开版本 1 日线缓存时在卡13 的每文件发布锁 + 结构性日志内逐条改写 `time=0`，最后把版本置 2（记 INFO）。tick/m1/m5 缓存仍只接受版本 1；日线缓存接受 1/2，其他版本仍 fail-closed。
- 读端：`WtDataReaderAD` 接受日线缓存版本 1/2，RT 缓存与 LMDB day 读出的日线 `time` 一律归一为 0；`WtRdmDtReaderAD`、`WtBtDtReaderAD` 的 day 读取同样归一。LMDB 值无逐条版本，只能读端归一；只读读取不改写旧文件。
- 兼容影响：改动前编译的读者/写者遇到版本 2 日线缓存会以 unsupported version 拒绝，须先升级再混跑（与卡13“旧 writer 不受协议保护”一致）。

### 3d（BUG-Q02-003）CtaMocker 冻结量：`src/WtBtCore/CtaMocker.cpp/.h`
- `dump_stradata` 每个持仓写 `frozen`；归属交易日即已存在的 `fund.tdate`。
- `load_incremental_data`：`frozen` 可选（缺省 0，兼容旧快照），须为有限数且在 `[0, max(volume,0)]`；读取 `fund.tdate`；任一持仓 `frozen≠0` 而无 `fund.tdate`（或为 0）时整份快照拒绝（沿用原“暂存后整体提交”规则，不部分恢复）。
- 新增 `_frozen_tdate`：`on_session_begin(curTDate)` 仅当 `curTDate != _frozen_tdate` 时清零，随后 `_frozen_tdate=curTDate`。无快照时与原行为一致（每个新交易日清零）；同一交易日再次 session-begin 不再清零（同日续跑）。
- 仅 CTA（Q02-003 范围）；SelMocker/HftMocker 的 frozen 未改、未验证。未改 HisDataReplayer。

## 2. 构建与运行

```sh
B=/tmp/wt-i03-card3-20261104; F=src/TestUnits/fixtures/i03_card3_dayboundary_20261104; LLVM=/opt/homebrew/Cellar/llvm/22.1.8/bin
cmake -S $B/baseline/src -B $B/core -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=$LLVM/clang++ "-DCMAKE_CXX_FLAGS=-include cstdlib" \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DWT_WARNINGS_AS_ERRORS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja -C $B/core -j4 WTSUtils WTSTools              # 两者不含本次改动
cmake -S $F -B $B/green-plain -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=$LLVM/clang++ -DCMAKE_C_COMPILER=$LLVM/clang \
  -DWT_LIB_DIR=$B/core/build_x64/Debug/libs -DWT_SAN=none -DWT_GREEN=ON
cmake -S $F -B $B/red-plain   ...同上... -DWT_GREEN=OFF -DWT_SRC_ROOT=$B/baseline/src
cmake -S $F -B $B/green-asan  ...同上... -DWT_SAN=address -DWT_GREEN=ON
ninja -C <dir> -j4
bash $F/run_cases.sh <dir>/<binary> <out> 120          # 逐用例独立进程/工作目录
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 <green-asan binary> --gtest_output=xml:...
# 3a ticker 探针（复用 /tmp/wt-i03-r202-001-midnight 旧隔离构建，仅重编三个 ticker TU 与测试 TU）
python3 $F/r202_run.py $B/r202-red    $B/baseline/src
python3 $F/r202_run.py $B/r202-green2 src
R202_TSAN=1 python3 $F/r202_run.py $B/r202-green-tsan src
# 卡9 9d：重编所有依赖 ticker 头的对象（tickers/三引擎/ParserAdapter/测试 TU），R202_REGRESS=1 另建既有 ticker 测试
R202_REGRESS=1 python3 $F/r202_run.py $B/r9d-red   $B/pre9d-root        # 红：9d 之前的本卡版本
R202_REGRESS=1 python3 $F/r202_run.py $B/r9d-base  $B/baseline/src      # 卡3 改动前
R202_REGRESS=1 python3 $F/r202_run.py $B/r9d-green src
R202_TSAN=1 R202_REGRESS=1 python3 $F/r202_run.py $B/r9d-green-tsan src  # 全部重编 TU 插桩 TSan
```

`r9d-green-tsan-failed-build.log`：首次 TSan 构建因 Homebrew clang 不搜索 `/usr/local/include`（rapidjson）编译失败，脚本加 `-isystem /usr/local/include` 后重跑；不是测试结果。

`r202-green`（无 `-2`）是加入 00:00 下溢修正之前的中间运行（NextDay 仍红），保留作过程记录，结论以 `r202-green2` 为准。

## 3. 用例与红绿（日志/XML 在 `evidence/`）

| 目标 | 覆盖 | 红（改动前快照） | 绿（工作区） |
|---|---|---|---|
| `c3_actday_ParserCTP/CTPMini/Femas`（fake 时钟） | R105 / 3b | 各 5 例中 3 例失败（00:30+本地 23:30→次日、00:30+本地 00:30→本地日、00:30+本地 05:15 丢弃），2 例正控通过；4 例 GREEN-ONLY 不编译 | 各 9/9 |
| `c3_ad_daybar` | R4-BUG-13 / 3c | 7/8 失败（新日线 time、版本 2、跨日 LMDB、v1 迁移、v2 读取、v1/LMDB 读端归一、Bt/Rdm 归一）；未知版本 3 拒绝正控通过 | 8/8 |
| `c3_cta_frozen` | Q02-003 / 3d | 4/6 失败（同日保留、dump frozen、缺 tdate 拒绝、超量拒绝）；跨日清零、旧快照两正控通过 | 6/6 |
| `r202_run.py` R202NextDay.Cta/Sel/Hft | R202 / 3a | 3/3 exit 1 | 3/3 exit 0 |
| C3TickerLeapSecond.Cta/Sel/Hft（23:59:60） | 3a | 3/3 exit 1（被交给引擎、secs=60000） | 3/3 exit 0 |
| R202Reentry.Cta/Sel/Hft（同步重入） | 9d | pre-9d 3/3 exit 1（worker 在重入返回后把时间写回 `2400`） | 9d 后 3/3 exit 0 |
| 既有 R202 ticker 正控 10 例 | 收盘/标签 | 10/10 | 10/10 |

既有相关测试（逐用例，同一产品源码，红/绿对照）：
- 三个 parser lane（t_ParserCTP 21、t_ParserCTPMini 27、t_ParserFemas 33，依赖墙钟）：红绿均全通过。
- AD（p05 writer/reader 相关 120 例）：红 119/120、绿 113/120。共同失败 `R122WriterADUpdateTickCacheUninitialized`（既有）。绿新增 6 例均为**旧合同断言**，与 3c 裁定直接冲突，**未修改断言**，待整合时按裁定更新：
  - `R4WriterADTest.BarCacheMergesTicksOfDayMinute5AndMinute1`（断言 `d1.time == 交易日`）
  - `R4WriterADTest.FreshInitBuildsFourBlockFilesWithValidADHeader`（断言 d1 头版本 1）
  - `R4WriterADTest.RejectsMalformedCacheFilesWithoutModifyingAnyCache` 的 `bad_version` 子例（把 d1 版本 2 当作非法）
  - `R4WriterADTest.L03AlignedBarCachesRecoverSecondRecordAndFlushPreviousBars`（d1 部分：v1 缓存被迁移为 time=0，逐字节比较不再相等；m1/m5 部分通过）
  - `P05BtReaderADTest.DayBarRoundTripFieldByField`、`P05BtReaderADTest.MalformedLmdbValuesAreRejectedWithoutChangingOutput`（断言 day `time == date`）
- q02_btcore3 CTA 29 例：红绿均 25/29，相同 4 例失败（`Q02CtaIntegrationTest` 的 rawcode/查询类，既有）；`onlyValidMatchesTotalBecauseFrozenNotPersisted` 在绿侧仍通过（其 JSON 无 frozen 字段）。
- 卡13 夹具（重编于 `$B/mw-plain`，含本次 writer 改动）：`mw_ad` 9/9；`mw_regress_ad` 63/68，失败为上列 4 例 writer 旧合同断言 + 既有 R122。
- 卡11 夹具（`$B/slice-plain`）：`i03_slice_unit` 6/6、`i03_slice_rdm` 9/9、`i03_slice_rdm_regress` 183/184（`Q03RdmKlineTest.day_tail_fragment_is_dropped`，卡11 README 记录的既有失败）。卡11 的 servo/replay/abi 目标需完整核心静态库，本次未构建。

卡9 9d（`r202_run.py`，红=pre-9d、绿=当前；`evidence/r9d-*/results.txt`）：

| 用例（各 CTA/SEL/HFT） | 卡3 改动前 | pre-9d（红） | 9d 后（绿） |
|---|---|---|---|
| R202Reentry | 1/1/1 | 1/1/1 | 0/0/0 |
| C3TickerReentry.WorkerQueuedInOrder（2400 回调内重入 3 笔：返回时 0 笔执行、FIFO 恰好一次、不写回 2400、00:01 闭合） | 1/1/1 | **124/124/124**（就地重入在 `_mtx` 上自死锁，35s 超时） | 0/0/0 |
| C3TickerReentry.StrategyQueuedInOrder（策略 on_tick 内两级嵌套重入） | 1/1/1 | 1/1/1 | 0/0/0 |
| C3TickerStop.StopWhileQueuedDrainsAndJoins（worker 回调中排队后另一线程 stop，<2s 且排队事件恰好一次） | 1/1/1 | 0/0/0 | 0/0/0 |
| C3TickerStop.WorkerStopDoesNotSelfJoin（worker 回调内 stop） | -6/-6/0 | -6/-6/0（CTA/SEL 自 join 抛异常终止） | 0/0/0 |
| R202NextDay、C3TickerLeapSecond | 1 | 0 | 0 |
| 既有 R202 ticker 正控 10 例 | 0 | 0 | 0 |

既有 ticker 测试（R202_REGRESS，37 例：r160 HFT、r170、r41、r66、n02 slicing、r181；`r202_run.py` 从工作区复制这些测试源码）：卡3 改动前 35/37；pre-9d 与 9d 后（r66 未更新时）均 29/37。

**首 tick 合同（用户裁定 2026-11-04）**：所有首 tick（冷启动首 tick、新交易日首 tick）都推进平台时间并交给策略（即本卡 3a 行为）。据此只改测试文件 `src/TestUnits/coverage/q01_wtcore3/r66_tickers_test.cpp` 中 6 例 CTA/SEL 的旧断言“第一笔只建立 `_cur_pos`、不调 `trigger_price`、平台时间保持 900”（改前 sha256 `ed2c08d4…7f`，改后 `d6435db1…20`）。新增测试内 `R66CountingEngine<Base>`：只按代码统计 `on_tick` 次数并转发给真实引擎。逐例改动：

| 用例 | 原断言（首 tick 后） | 新断言（首 tick 后，及后续收紧） |
|---|---|---|
| `R66CtaTickerThreadedSameMinuteDispatch` | `get_min_time()==900`、未闭合 | 主力/热合约各恰好投递 1 次、价格 3500、日期 20991231、时间 902、秒 0、未闭合；同分钟第二笔后计数 2、仍未闭合（证明分钟位已由首 tick 建立） |
| `R66CtaTickerThreadedMinuteCloseSchedulesAndReplays` | 时间 900、未闭合 | 恰好 1 次、价格 3500、日期 20991231、时间 903、未闭合、未调度；下一分钟 tick 闭合的正是 903（首 tick 的分钟位），计数 2 |
| `R66CtaTickerSectionEndTickDoesNotCloseEarly` | 时间 900 | 恰好 1 次、价格 3500、日期 20991231、时间 1500、未闭合；15:00 第二笔仍同一分钟位不闭合，计数 2 |
| `R66SelTickerThreadedMinuteCloseReplays` | 时间 900 | 主力/热合约各 1 次、价格 3500、日期 20991231、时间 902、未闭合；下一分钟闭合 902，计数 2 |
| `R66SelTickerFlatContractSkipsHotLoop` | 时间 900（注释称首笔不走 trigger_price） | 首笔即走 trigger_price：主力 1 次、热合约 0 次、价格 480、热合约价 0、日期 20991231、时间 932、未闭合；第二笔后主力 2 次、热合约仍 0 |
| `R66SelTickerSectionEndAndStaleTick` | 时间 900 | 恰好 1 次、价格 3500、日期 20991231、时间 1500、未闭合；15:00 第二笔不闭合，计数 2 |

原有后续断言（闭合标签、调度、热合约价格、旧日期短路）全部保留未放宽。更新后：工作区（9d 后）37 例中 35 通过（`evidence/r66u-green/`），失败仅为下述 2 例既有 HFT。 红控：同一更新后的 r66 用卡3 改动前的 ticker（`evidence/r66u-base/`）这 6 例全部失败（exit 1），证明新断言能区分新旧行为。TSan（`evidence/r66u-green-tsan/`，全部重编 TU 插桩）68 个进程 0 个 ThreadSanitizer 报告，66/68，失败同为 2 例既有 HFT。

**2 例既有 `R66Hft*` 失败的原因**（未改动）：`R66HftTickerThreadedMinuteCloseReplays`、`R66HftTickerSectionEndAndStaleTick` 依赖 `armThreadedMode` 的“先 `stop()` 再 `run()`”技巧来得到一个非空 `_thrd`。HFT ticker 在提交 `e03cbe1d`（迭代02）改为单次启动生命周期：`run()` 见到 `_stopped` 已置位即直接进入 `Joined`、不创建 worker，此后 `on_tick` 走“无 worker → 只 `trigger_price`”分支，不做分钟切片，因此平台时间停在 900、`reader.count==0`。卡3 改动前、pre-9d、9d 后三侧失败断言完全相同，与本卡无关；修复需改测试的布线方式（例如不经 stop 直接 run，并处理 worker 时序）或另行裁定，本次未处理。

9d TSan（`r9d-green-tsan`：三个 ticker、三个引擎、ParserAdapter 与全部测试 TU 均 `-fsanitize=thread`，`halt_on_error=1`）：68 个进程 0 个 ThreadSanitizer 报告，`_date/_time` 竞争消失；退出码与 plain 相同（仅上述 8 例 r66 断言失败，exit 1）。重复：21 个 R202/C3 ticker 用例 plain×5、TSan×5 全部通过，0 报告（`evidence/r9d-repeat/`）。

Sanitizer：`green-asan`（ASan+UBSan，排除 alignment，`detect_leaks=0`）5 个 c3 目标全部通过、无报告。pre-9d TSan（`r202-green-tsan`，仅 ticker TU 与测试 TU 插桩）6 个进程均以 `-6` 终止，首个报告为 `on_tick` 写 `_date/_time`（WtCtaTicker.cpp:128）与 worker 读 `_time`（:234）——作为 9d 的 TSan 红控；9d 后见上。

警告：三个 ticker TU 在旧隔离构建的 `-Wall -Wextra -Werror` 下编译通过；`CtaMocker.cpp`、四个 AD TU 以 `-Wall -Wextra -Werror -fsyntax-only` 通过；三个 parser TU 仅有厂商 SDK 头 `unused-parameter` 诊断，红侧相同（`evidence/werror-nonvendor.log` 过滤厂商头后为空）。

## 4. 限制

仅本机 macOS 局部验证。3b 未取得 CTP/CTPMini/Femas 精确 SDK 的 ActionDay/TradingDay 证据（外部 X），fake 时钟不代表真实前置。ticker 探针中依赖 ticker 头的对象均已重编，但其余引擎对象、静态库和依赖仍来自 2026-11-02 旧隔离构建（旧快照的引擎源码），不是当前完整树；真实 provider 的回调线程模型、多行情线程高负载下的阻塞时延、Windows 未验证；2 例既有 R66Hft 失败未处理。3c 未覆盖旧二进制与新二进制混跑、Windows、真实历史数据；6 条旧断言待整合更新。3d 仅 CTA。整体回归 NOT_RUN。
