# 2026-11-04 S5 子任务 date-session：日期映射失败传播、Session 语义、时区失败

关联条目：BUG-R12-13、BUG-L01-018（本机部分）、BUG-L03-009、BUG-L03-010、BUG-L03-002（仅两个 commodity 旧用例）、I03-NEW-006、I03-NEW-011（主线程追加）。合同依据：卡 1=A+C；卡 2 用户确认规则（竞价 `(0,2400)` 全天；交易段仅 `0–0` 全天、其他相同起止拒绝；offset 运行时归一化、配置拒绝 |offset|>1440）；卡 4a=A（含 2026-11-04 trademode 补充裁定）。

- 起始 HEAD：`HEAD.txt`（ccaad010…）；起始时间 `start-time.txt`。工作区有大量他人未提交改动；读端、Replayer、ParserXeleSkt 的其余部分由其他包并行修改。
- 改前副本：`before/`（保留相对路径），哈希 `before.sha256`（每个文件在本包首次改动前即时复制）；逐文件差异 `diffs/`（相对改前副本，读端文件的差异只含本包改动）。
- 本包未修改任何跟踪文件；未创建符号链接；未运行 git 改动类命令；测试只用本地 fake/loopback。

## 1. 改动摘要

### 1.1 卡 1（A+C）调用者迁移 —— BUG-R12-13 / L03-009

- 新增 `src/Includes/DateMappingClient.hpp`：typed 结果客户端。来源二选一：从具体 `WTSBaseDataMgr` 显式取得的 `DateMappingV1` 表（A），或任意 `IBaseDataMgr*` 的两个旧 virtual（C：调用前校验 id/HHMM/公历日期、商品/会话非空且有交易段、日历名非空；调用后校验返回日期/边界合法，异常转 `InternalError`）。`date=0`：A 路径在同一客户端内只取一次 checked 快照并以显式日期调用；C 路径保留旧含义“管理器自己的当前日期”（模拟/回放管理器或测试替身可与挂钟不同），只要求先能取得 checked 时钟快照，并校验返回值（返回 0/非法日期即失败）。时钟失败为 `ClockFailure`。未改 `IBaseDataMgr` vtable、`DateMappingV1.h`、`WTSBaseDataMgr`。C 路径无法发现“日历未加载”（`IBaseDataMgr` 无查询），见决策卡草稿 D4。
  - 过程记录：首版 C 路径把 date=0 替换为挂钟快照，coverage 读端回归（`runs/` 未归档的 lanes1）中 q03 读端 46 例、R1ReaderBars 等因测试替身以 date=0 注入“当前交易日”而失败；据此改为透传 0，复跑见 2.2。
- 读端（只改日期映射调用点，保持原 CRLF 行尾）：`WtRdmDtReader.cpp`（4 组三连映射、3 组两连、按日读、`cacheHisBarsFromFile` 当前交易日与主力分段边界）、`WtDataReader.cpp`（5 组、`cacheIntegratedBars` 当前交易日与分段边界；两处从未使用的映射与未校验时钟读取删除）、`WtDataReaderAD.cpp`（2 处）、`WtRdmDtReaderAD.cpp`（区间、按日、K 线区间）。映射失败记 ERROR 并在建切片、写缓存前返回 NULL/false；边界失败按既有失败路径释放分段并清除该键缓存。`WtBtDtReader*.cpp` 无日期映射调用。
- `HisDataReplayer.cpp`（只改日期映射调用点）：K 线回放换日映射、HFT/普通会话开盘边界、HFT flush 会话边界（删除 boundary=0 时用自然日+区间起点的兜底）、任务回放起点/换日/次交易日改用 checked 接口，失败报 `WT_REPLAY_ERROR_DATE_MAPPING` 并终止，不再发 session 回调；`cacheIntegratedFutBarsFromBin` 用单次 checked 快照；集成分段边界改 checked；两处未使用映射删除。已有 0 判定并报错的 HFT 路径（自然时刻映射/边界）未改。
- `ParserXeleSkt.cpp`：行情帧在改写缓存行情前完成映射；失败（缺商品/会话、旧接口返回 0 或非法日期、抛异常）记 ERROR 并丢弃该帧，不写 action/trading 日期、不投递。
- `StateMonitor.cpp/.h`：运行循环改为一次 `tryGetDateTime` 快照（原来日期、分钟两次读取）；失败时不推进状态、不触发收盘作业或缓存清理，失败/恢复各记一次；`advanceOnce` 对 date=0、非法日期/时间、越界年份直接返回，偏移日期改 `tryGetOffsetDate`、前一日改 `tryGetNextDate`；头文件仅加友元 `I03MonitorDateTestAccess` 与注释。初始化部分沿用 2026-11-03 已迁移实现。
- `WtDtServo/WtDataManager.cpp`：日期映射调用点已于 2026-11-03 迁移（证据 `../20261103-servo-date-review/`），本包未再改。

### 1.2 I03-NEW-006 时区/时钟失败

`Share/TimeUtils.hpp`（保持原混合行尾）：旧 `getLocalTime/getYYYYMMDDhhmmss/getDateTime/getCurDate/getCurMin` 改为经 `legacyLocalNow`：检查 `clock_gettime`；时区缓存失败（哨兵 99）时改用平台本地时间转换，绝不把 99 当小时偏移；时钟或转换失败时发布全零 tm（日期 19000100、时间 0，非 0 且非法）。`makeTime(…, true)` 在 99 时返回 0。新增 `tryGetNextDate`（纯公历、与时区/DST/mktime 无关），`getNextDate` 对合法日期改用它（中国时区结果不变，DST 时区更正），非法输入保留旧 mktime 规范化路径。旧接口仍无失败位，关键调用方需迁移到 `tryGetDateTime`（见第 4 节）。

### 1.3 I03-NEW-011（ParserXeleSkt `strToTime`）

删除 `static char str[10]` 与 `strlen`：`tryParseXeleClockField` 只在字段容量内扫描、要求容量内 NUL 结尾、只接受数字与 `:`、至多 8 位；空字段仍为 0。格式错误时该快照的 action 日期/时间置 0 并记 ERROR（保留合约，首个行情帧再由 SnapDateTime 赋值），不越界、不共享状态。

### 1.4 卡 2 / BUG-L03-010

`WTSSessionInfo::tryAddTradingSection` 拒绝 0–0 与其他段混合（任一顺序；配置入口经 `loadSessions` 整份不发布），保证全天聚合视图恰为一天。曾试做“普通段内部重叠拒绝”，因旧用例 `l03_session.set_offset_mins_keeps_registered_sections_consistent` 依赖重叠追加且合同未定，已撤回为决策卡 D3。空会话、跨零点 autoAdjust 空档见 D1、D2。

### 1.5 BUG-L03-002 两个旧用例（卡 4a=A）

- `l03_contract.storage_width_contract_limits_preserve_valid_edges_and_reject_before_rules_registration`：旧用例的 rules 无 pricetick/volscale、未加载 ALLDAY 会话，且期望混合文件中合法半份被登记，三点均与卡 4a=A（只登记就绪商品、预校验失败整份不发布）冲突。改为加载 ALLDAY、rules 补 `pricetick/volscale`，混合文件期望整份不发布（合法合约/商品均不登记、全局数 0）；宽度上限断言不变，且无效用例除宽度外均就绪。
- `fixtures/l03_commodity_state/init.cpp`：未配置商品的有效交易模式为 `TM_None`、不可做空、未就绪；`setTradingMode()` 会写“已配置”状态位，因此从“对象表示不变”清单移出单独断言（先断言 TM_None，再显式设 TM_Both 后可做空但未就绪）。

### 1.6 其他测试改动

- `fixtures/i03_session_contract_20261103/check.cpp`（本包夹具）`I03SessionContract.TimeQueriesRequireValidHhmm`：原在已有 0900–1000 的会话上追加 0–0，与卡 2 混合拒绝冲突；改为断言追加被拒，再用独立会话做全天查询，原全天断言保留。
- `l03_session_info_test.cpp` 新增 `l03_session.full_day_section_is_not_mixed_with_other_sections`。
- 新夹具 `src/TestUnits/fixtures/i03_date_mapping_20261104s5/`（见其 README）。

## 2. 验证

所有运行均在独立 cwd（`/tmp/wt-i03-s5-date-session-run/...`）、`perl alarm` 硬超时下进行；覆盖率二进制设 `LLVM_PROFILE_FILE` 到临时目录。机器在验证期间负载 60–200（12 个并行子任务），耗时数据不具参考意义。

### 2.1 新夹具红绿对照（`runs/fixture5/`，最终版本）

构建：`sh src/TestUnits/fixtures/i03_date_mapping_20261104s5/run.sh`（Homebrew LLVM 22.1.8；ASan+UBSan，TSan 单列；改前源码取自 `before/`）。

| 可执行文件 | 当前 | 改前（红控） |
|---|---|---|
| date_client | 10/10 exit 0 | 不适用（新接口） |
| tz_legacy | 2/2 exit 0 | 0/2 exit 1：时区查询失败时 `getCurDate()` 为 20261108（实际 20261104，+99 小时）；`getNextDate(20240229,1)=19700101`；时钟失败仍返回貌似合法日期 |
| monitor_chain | 3/3 exit 0 | 2/3 exit 1：非法日期 20240230、date=0、非法时间等快照仍推动状态离开 ORIGINAL |
| reader_chain | 4/4 exit 0 | 1/4 exit 1：非法端点/缺会话/时钟失败时旧读端仍把非法日期交给旧 virtual 并返回切片 |
| xele_chain | 5/5 exit 0 | exit 134：映射失败时投递 `trading_date` 错误/为 0 的行情；`strToTime` UBSan `index 10 out of bounds for type 'char[10]'` |
| xele_tsan（并发 prepare） | 1/1 exit 0，无 TSan 报告 | exit 134：`ThreadSanitizer: data race`（`ParserXeleSkt::prepare` 内静态缓冲） |

### 2.2 覆盖率构建（`/tmp/wt-i03-s5-date-session-cov`，默认 `-Werror`）中的关联用例

目标 `CoverageTests`、`CoverageTests_t_ParserXeleSkt`、`CoverageTests_dtcore`、`CoverageTests_adstorage` 构建 exit 0（主目标二进制哈希 `coverage-main-binary.sha256`）。脚本 `scripts/run_lanes.sh`，结果 `runs/lanes2/`、`runs/q05-monitor/`、`runs/ad-readers/`、`runs/xele-lane/`。

| 组 | 结果 | 失败归因 |
|---|---|---|
| l01_timeutils.* | 24/24 | — |
| l03_*（含两例 L03-002 更新用例、新增混合 0–0 用例） | 67/68 | `l03_object.release_is_idempotent_at_zero_refs`（WTSObject 对象池计数，本包未改相关文件，非本包） |
| r12_session.* + R12BaseDataMgr* | 13/21 | 8 例均为 fixture `loadSessions` 失败：配置含无 `sections` 的 `R12EMPTY`，被 2026-11-03 卡 2 整批校验拒绝（旧 oracle 冲突，非本次改动） |
| R1BaseDataMgr* + R1RdmHft + R1ReaderBars | 26/40 | 14 例为 `loadSessions`（`BADSEC` 无 sections）/`loadCommodities`（卡 4a 缺字段）前置失败，同上 |
| P09*（WtRdmDtReader/WtDataReader） | 32/33 | `P09WindowTest.public_kline_slice_readers_keep_empty_and_nonempty_results`：替身 `calcTradingDate(…,0,…)` 返回 0（非法“当前交易日”），按卡 1 C 现为映射失败 → 需 storage-read 包把替身改为返回合法日期 |
| Q03/R100/R112/R122/R132/R142/R162/R172/R182/R193/R203/R212/R52/R70/R74 读端 | 170/171 | `Q03RdmKlineTest.day_tail_fragment_is_dropped`：既有失败（`../20261104-slice-shared-owner-review/` 记“基线同样失败”） |
| R23Rdm* + R52RdmTest | 25/25 | — |
| StateMonitor 相关（dtcore） | 26/56 | 30 例均为夹具前置失败：`loadCommodities`（卡 4a 缺 pricetick/volscale）、`loadSessions`（`NODATA` 无 sections、`0–2400` 交易段被卡 2 拒绝）、`initialize`（未加载节假日，2026-11-03 初始化迁移后为 MissingCalendar）；均非本次改动，需按卡 2/4a/1 更新旧夹具。运行循环类 R145 用例通过 |
| AD 读端（adstorage） | 165/165 + R122ReaderADTest 8/8 | — |
| t_ParserXeleSkt 全 lane | 34/34 | — |

（btcore / dtservo 结果见 2.4；I03-NEW-043 见 2.5。）

### 2.3 其他复跑

- `runs/l03-standalone/`：独立链接 l03 会话/合约/商品 + l01 时间用例 82/82；storage_width 改前用例对当前产品 1 失败（第 253 行 `loadContracts` 为 false）、改后 1/1；新增混合 0–0 用例用改前头文件编译 0/1（红控）。
- `runs/l03-init/`：`l03_commodity_state/init.cpp` 改前对当前头文件普通/ASan+UBSan 均 `FAIL: default initialization` exit 1；改后两变体 PASS exit 0（`run-init.sh`）。
- `runs/dm1103/`：2026-11-03 日期映射夹具 5/5。
- `runs/sc1103b/`：2026-11-03 Session 合同夹具（含 l03_session/L03DaySession/r12_session）49/49。`runs/sc1103/` 为试做“普通段内部重叠拒绝”时的运行（2 失败，促成撤回为仅拒绝混合 0–0，见 1.4 与决策卡 D3）。
- `runs/xelev2-impact/`：dt-parsers 夹具 `i03_parser_xele_lifecycle_v2` 的 5 个场景对当前模块 0/5（替身 `getCommodity` 返回空、商品无会话 → 按卡 1 C 丢帧，等不到行情）；仅把替身改为返回带会话的商品（`suggested-fake.diff`，未改仓库文件）后 5/5。`i03_parser_event_contract_20261103/events.cpp` 复用同一替身，预计同样受影响（未运行）。

### 2.4 btcore / dtservo

- btcore（`runs/btcore/`，构建 exit 0）：回放相关组 P04/R43/P07/R67/ReplayTimeRange/R103/R2/R80/R91/S5Q02/S5R103/HisDataReplayerRuntime 共 276 例，220 过 56 败。失败全部发生在 `replayer.prepare()`/`replayer.init()` 返回 false 及其后续断言，本包未改 `prepare/init`；其中 25 例在 btcore 包自己的基线回归 `../20261104-s5-btcore/runs/red-reg/results.txt` 同样失败；其余 31 例（R43ReplayerTickCsv 18、P07TickLoader 9、R2Public 3 等）其基础配置未提供 `holiday`（同 I03-NEW-018 归因：2026-11-03 起回放起点 checked 映射要求日历）或在 `init` 失败，btcore 包运行未覆盖。属静态归因，未构建去除本包改动的 btcore 对照二进制。
- dtservo（`runs/servo/`，构建 exit 0）：Q05ServoWtDataManager/R102/R133/R8WtDataManagerSinks/R78DtDataManager 24/24（2026-11-03 已迁移的 `get_kline_slice_by_date` 调用点回归）。

### 2.5 I03-NEW-043（主线程追加）：q05 StateMonitor 夹具合同漂移

- `q05_dtsrv/r5_state_monitor_bdt_test.cpp`（改前 `before/`，差异 `diffs/`）：共享 sessions 文件中的 `NODATA`（缺 sections）、`R5EQUAL`（120-120）、`R5ALLDAY`（0-2400）在卡 2 整批校验下使整份不发布 → 移出 NODATA/R5EQUAL、全天改为 0–0（StateMonitor 换算区间不变），新增 `InvalidSessionEntriesRejectWholeFile` 断言三种无效条目各自使整份不发布且既有会话不变、监控器无法引用被拒会话；商品补 `pricetick/volscale`（卡 4a=A）；初始化含品种会话的用例先加载 `SSE_H/SHFE_H/LONG_H`（卡 1：checked 映射要求日历）；`R5CROSSOVERLAP` 偏移 -1440 后结束端点按右对齐为 2400（卡 2c 归一化 + `offsetTime(…,false)` 既有约定，原期望 0）；非对象会话项按 2026-11-03 起的 fail-closed 整批初始化改为期望初始化失败且不发布，并加“失败的重新初始化不破坏既有状态”断言。原 R5EQUAL 的相等端点 1 分钟填充断言因该段已无法注册而删除。未放宽任何产品校验。
- `q05_dtsrv/r44_datamgr_caster_edges_test.cpp`：SetUp 未加载商品引用的 `CHINA` 节假日 → StateMonitor 初始化 MissingCalendar；补空节假日模板。
- 结果（`runs/q05-r5-r44/`，覆盖率构建）：R5StateMonitorBdtTest 14/14（另 1 例原有 DISABLED）；R44DataManagerEdgesTest 4/6，SetUp 全部通过，剩余 2 例 `InitFailsOnExistingButCorruptDefaultModuleFile`、`InitFailsWhenDefaultModuleNameIsDirectory` 断言 DataManager 初始化失败后 `getBDMgr()` 仍为传入管理器（现为 NULL）及 `canSessionReceive` 为假（现为真），属 DataManager 失败初始化发布语义（dt-writer/卡 9 范围），未改。独立链接对照（`runs/r5-standalone/`）：改前用例 0/13，改后 14/14。
- 同源但未分配给本包的旧夹具：`r68_state_monitor_inputs_test.cpp`（未加载节假日、非对象项期望默认 0）、`r84b_*`、`r154_*`、`r164_*`（商品缺 pricetick/volscale）——见 2.2 表与第 4 节。

## 3. 修改文件与哈希

`before.sha256`（改前）、`hashes-after.txt`（前/后对照，归档时刻）、`fixture-and-new-files.sha256`（新文件）。`HisDataReplayer.cpp` 在本包改动后于 20:24 又被 btcore 包修改，归档差异中混有其改动；本包改动仅为 1.1 所列调用点（`checkedCalcTradingDate/checkedGetBoundaryTime/checkedNextTradingDate` 与 `WT_REPLAY_ERROR_DATE_MAPPING` 处）。btcore 回归运行使用的二进制早于该修改。读端与 `TimeUtils.hpp` 首次写回时被脚本误转为 LF，已用 `difflib` 按改前副本逐行恢复原 CRLF/混合行尾（`diffs/` 为恢复后的最小差异）；恢复前编译产物已作废重编。

## 4. 未闭合与需其他包的改动（只记录，未改）

1. 卡 1 其余生产调用点：`WtCore/WtCtaTicker.cpp:234`、`WtHftTicker.cpp:274`、`WtSelTicker.cpp:234`、`WtUftCore/WtUftTicker.cpp:288` 对 `IBaseDataMgr*` 调 `calcTradingDate(session, engine date, min, true)` 后直接 `set_trading_date`，应改用 `DateMappingClient`（或宿主持有的 V1 表）并在失败时不启动/不设交易日；`WtDtCore/DataManager::getTradingDate`→`WTSBaseDataMgr::getTradingDate`（旧接口，含当前日期兜底与缓存副作用）被 `WtDataWriter.cpp:3531` 用于日线日期，应迁移 `checkedGetTradingDate` 并让 writer 处理失败；`TraderCTPMini.cpp:713/1233-1245/1347-1359`（`GetTradingDay` 无校验、夜盘下单日期用 `getWeekDay/getNextDate`，无效日期走旧 mktime 路径）需用 `tryGetWeekDay/tryGetNextDate` 并在登录处校验（T3-015，SDK 合同为 X）。
2. I03-NEW-006 其余调用方（审计第 3–6 项）：引擎/ticker 构造期日期、TraderAdapter 本地 ID 种子、CTPOpt 结算确认、`WtDataWriter` 日期选择仍用旧接口；本包的 `TimeUtils` 加固只保证不再出现 +99 小时偏移、失败时发布非法日期 19000100，调用方仍须迁移 `tryGetDateTime` 才能在副作用前停止。
3. 旧夹具/oracle（非本包 lane）：`r12_headers/r12_base_datamgr_test.cpp`（R12EMPTY 无 sections）、`r1_storage/r1_basedatamgr_test.cpp`（BADSEC、商品缺字段）、`q05_dtsrv` 的 R5/R68/R84b/R154/R164（无 sections、`0–2400` 交易段、商品缺字段、未加载节假日）、`p09_storage_readers` 替身 `calcTradingDate(…,0)` 返回 0、`fixtures/i03_parser_xele_lifecycle_v2/xele_fixture.hpp` 替身无商品/会话（`runs/xelev2-impact/suggested-fake.diff`）。
4. 疑似新缺陷（静态分析）：`WtRdmDtReaderAD::readTickSliceByRange` 把已按会话偏移过的时间（`sInfo->offsetTime(...)`）传给 `calcTradingDate`，后者期望原始时间并再做偏移；对 +300 夜盘 22:00 的端点会得到自然日而非次交易日，与 writer 以（交易日、偏移时间）建键不一致，可能漏读夜盘 tick。本包保持原参数不变，需 storage-read 包构造夜盘红控后修。读端在 `commInfo` 为空时仍先解引用（`commInfo->getFullPid()`），在到达日期映射前崩溃，属 storage-read 范围。
5. Replayer：`get_hft_trading_date_range` 的 bool 仍混合“无订阅”与“映射失败”，bar+HFT 路径把失败当无 HFT 继续；已有 0 判定的 HFT 自然时刻/边界路径未改为 checked；`get_daily_bar_close_time` 的 boundary=0 回退保留。外部完成通知无错误码。
6. Windows/MSVC 日期矩阵、真实 SDK/柜台、DST 实地切换、整体回归均未验证；全量回归 NOT_RUN。

## 5. I03-NEW-053 / I03-NEW-054（主线程追加：测试侧迁移）

只改测试/夹具补齐配置；另有一处产品诊断修复（见 5.2），未放宽任何产品校验。改前副本均在 `before/`，差异在 `diffs/`。

### 5.1 逐文件改动与合同依据

| 文件 | 改动 | 依据 |
|---|---|---|
| `r12_headers/r12_base_datamgr_test.cpp` | `R12EMPTY` 补 `"sections":[]`（保留“无时段”语义） | 卡 2：会话条目须有 sections 数组，缺失使整份不发布 |
| `r1_storage/r1_basedatamgr_test.cpp` | `BADSEC`、`nosec/ghost`、`BADPROD` 移出共享夹具，改为各用例单独加载“合法条目 + 无效条目”文件并断言整份不发布、同文件合法条目不登记、既有对象不变；`commodity_config` 补 pricetick/volscale；边界用例：空 exchg/pid/name/session 商品期望拒绝并给出原因（空 holiday 仍登记），空 exchg/product 的合约随商品不就绪整份不发布 | 卡 2 整批校验；卡 4a=A（只登记就绪商品、预校验失败整份不发布；就绪要求名称/交易所/品种/币种/会话非空、正 pricetick、正整数 volscale） |
| `q05_dtsrv/r68_state_monitor_inputs_test.cpp` | 数字/布尔会话项改为期望初始化失败、不发布，并补“非法重新初始化不破坏已发布状态” | 卡 1=A+C 入口前置校验兜底及 2026-11-03 已复核的监控器“整批校验后发布”；非对象项若以全 0 时间登记会令状态机在 00:00 收盘/作业 |
| `q05_dtsrv/r84b_state_monitor_positive_offset_test.cpp` | 商品补 pricetick/volscale；非对象项用例同 R68 改写，且断言预置交易日缓存不被改写 | 卡 4a=A；同上 |
| `q05_dtsrv/r154_test_util.h` | 商品补字段；`StateFixture::SetUp` 加载空 `R154_NONE` 节假日 | 卡 4a=A；卡 1（监控器 checked 映射要求日历） |
| `q05_dtsrv/r164_state_monitor_holiday_test.cpp` | 商品补字段；竞价用例加载空 `R164_NONE` | 同上 |
| `p09_storage_readers/p09_rdm_window_range_test.cpp` | 替身 `calcTradingDate(…,0,…)` 返回固定合法当前交易日 20201231（原回吐 0） | 卡 1 C 路径：0 不是合法交易日，读端视为映射失败 |
| `fixtures/i03_parser_xele_lifecycle_v2/xele_fixture.hpp` | 替身 `getCommodity` 返回挂会话的商品（主线程授权） | 卡 1 C 路径：ParserXeleSkt 先校验商品/会话再映射 |
| `l03_includes/l03_object_pool_test.cpp` | I03-NEW-054：两例按用例开始时的基线比较 `g_tracked_dtor` | 见 5.3 |

`r12_base_datamgr_test.cpp` 中“未知品种原样回吐入参日期”等旧接口断言未改：旧 virtual 行为不变（卡 1 只承诺合法输入），断言仍成立。

### 5.2 产品诊断修复（`WTSTools/WTSBaseDataMgr.cpp`，本包所有）

卡 4a 的整批预校验在合约身份超宽时 `return false` 不记任何日志、商品身份超宽只记 “invalid identity”，使既有边界用例对拒绝原因的断言（"Contract identity exceeds storage field capacity" / "Contract identity exceeds field capacity" / "Commodity identity exceeds field capacity"）失败。改为在预校验拒绝时输出与登记阶段一致的诊断（并注明 nothing loaded），拒绝行为不变。红控：新用例 + 改前 `WTSBaseDataMgr.cpp` 独立链接，`R1BaseDataMgrConfigBoundary` 0/3（20 处 `logs.contains` 失败，`runs/new053/r1r12/r1_red_diag.*`）；改后 R1 独立 17/17、R12 独立 8/8（`runs/new053/r1r12/`）。

### 5.3 I03-NEW-054 归因

`l03_object.release_is_idempotent_at_zero_refs` 断言进程级计数 `g_tracked_dtor == 0`，而同套件先运行的 `initial_ref_is_single`、`retain_and_release_balancing` 各析构一次；单独运行该例通过、按套件运行失败（覆盖率二进制复现：单例 1/1、`l03_object.*` 3/4）。`WTSObject.hpp` 与该测试文件相对 HEAD 无改动，属测试对执行顺序的依赖，非产品缺陷；改为相对基线断言（断言强度不变）。

### 5.4 夹具复跑

- `i03_parser_xele_lifecycle_v2`（`/tmp/wt-i03-s5-date-session-xelev2`，含 Fault 模块）：scenarios 5/5、v2 check 11/11、prepare 1/1（`runs/new053/xelev2-final/`）；改前替身对当前模块 scenarios 0/5（`runs/xelev2-impact/`）。
- `i03_parser_event_contract_20261103`（`/tmp/wt-i03-s5-date-session-events`）：13/13（`runs/new053/events/`）。

### 5.5 覆盖率构建复跑（I03-NEW-053/054 改动后，默认 -Werror，`CoverageTests`、`CoverageTests_dtcore` 构建 exit 0）

- 主目标（`runs/new053/lanes3/`，二进制哈希 `binary.sha256`）：l03 68/68、r12_session+R12BaseDataMgr 21/21、R1BaseDataMgr*+R1RdmHft+R1ReaderBars 40/40、P09* 33/33、l01_timeutils 24/24。
- dtcore（`runs/new053/q05-final/`）：Q05StateMonitor 8、R145 8、R154 6、R164 2、R174 3、R44DataManagerEdges 6、R44StateMonitorLifecycle 4、R22 1、R5 14、R68 4、R84b 7，共 63/63，无跳过。R44DataManagerEdges 先前剩余的 2 例（DataManager 初始化失败后的发布语义）在本次重编中通过，期间 WtDtCore 由其他包继续修改，本包未改 DataManager。
- 本包在本会话中删除过的路径（均为本包自建）：`/tmp/wt-i03-s5-date-session-fixture`（首次配置前）、`/tmp/wt-i03-s5-date-session-dm1103`、`/tmp/wt-i03-s5-date-session-sc1103`、`/tmp/wt-i03-s5-date-session-xelev2`、`/tmp/wt-i03-s5-date-session-events`（各自创建前清空）、`/tmp/wt-i03-s5-date-session-run/` 下的若干运行子目录、`/tmp/wt-i03-s5-date-session-r1r12/WTSBaseDataMgr_before.cpp`，以及本证据目录内被取代的 `runs/fixture2-4`。未删除任何其他目录或文件。
