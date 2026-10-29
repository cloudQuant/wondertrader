# R1 缺陷清单 — WtDataStorage / WTSDataDef / WTSBaseDataMgr

发现者：lane R1（覆盖率第七波）。所有条目均**未修改产品代码**，只记录。

---

## BUG-R1-001（原第 1 项）WTSTickSlice::_count 在 create(code) 单参路径下未初始化（与 BUG-R12-06 重复）
- 状态: 已合并（主记录 BUG-R12-06）
- 位置: `src/Includes/WTSDataDef.hpp` L1398-1418（构造器 `WTSTickSlice() { _blocks.clear(); }` 不初始化 `_count`；`create` 仅在 `ticks != NULL` 时赋值 `_count`）
- 触发: `WTSTickSlice::create("x")` 后调用 `size()` / `empty()` / `at(0)`
- 影响: 空创建后 `size()`/`empty()`/`at()` 读取未初始化的 `_count`；随后 `appendBlock` 还会以 `_count += count` 读取该值。属于未初始化读取 UB；MSan 可用于检测，ASan 本身不检测未初始化值。
- 建议: 构造器补 `_count = 0`
- 复核裁定（2026-10-29）: 源码缺陷成立，但与 `R12_headers_wtstools.md` 的 BUG-R12-06 是同一字段、同一构造与同一创建路径，R12-06 作 canonical，本条为重复记录。源码调用点包括 `WtRdmDtReader.cpp:391` 的空建后 append 路径和 `:2414` 的单参创建；本轮未运行 MSan/R12 disabled 用例。修复方向仍为构造时将 `_count` 初始化为 0，并保留空创建与 append 覆盖。

## BUG-R1-002（原第 2 项）WtRdmDtReader 的逐笔委托/成交历史缓存查错 map（缓存永不命中 + 顺序相关的假 NULL）
- 状态: 源码缺陷确认；同 key 有效文件回归未运行
- 位置: `src/WtDataStorage/WtRdmDtReader.cpp`
  - readOrdDtlSliceByRange his 分支 L876：`_his_ordque_map.find(key)`，但装载进 `_his_orddtl_map[key]`（L884、L913）
  - readTransSliceByRange his 分支 L1051：同样查 `_his_ordque_map`，装载进 `_his_trans_map`（L1059、L1088）
- 触发/后果:
  1) dtl/trans 缓存**永远不会通过当前 find 命中**（find 的 map 与 add 的 map 不同），每次调用重复读盘解压；
  2) 若同一 key 在 `_his_ordque_map` 有 queue 缓存、但目标 `_his_orddtl_map`/`_his_trans_map` 尚无对应条目，则错误的 queue 命中会跳过读盘；随后目标 map 默认构造空 pair 并返回 NULL（L913-915、L1087-1089），即使目标 `.dsb` 文件存在也会漏数。若目标 map 已先前装载，则这一分支不会返回空。
- 复现: 需要同一 reader、同一 `stdCode-endTDate` key：先读 queue，再读尚未读过的 dtl/trans 文件可观测顺序相关 NULL；仅先后调用不同 key 不会触发。现有 `orddtl_his_first_read_roundtrip` 与 `trans_his_first_read_roundtrip` 是首次读取测试，本轮运行均通过，但未覆盖 cache hit 或 queue-first 顺序。
- 建议: find 改为各自的 `_his_orddtl_map` / `_his_trans_map`
- 复核裁定（2026-10-29）: 产品缺陷（查错 map）；L876/L1051 与目标 map 装载位置直接不一致。R100/R112 旧测试只写 queue 而未创建目标文件，返回 NULL 不构成串扰证据；现已改为独立缺文件/短文件控制，二者定向 PASS。新的三类合法文件、同 key 顺序回归保持 DISABLED / NOT_RUN：CMP_V2 路径存在旧指针风险，R100 原组合测试曾 signal 11 但无栈，不将此次信号归因于该风险。最小产品修复是分别查 `_his_orddtl_map`、`_his_trans_map`，修复安全前置后再启用有效文件回归。

## BUG-R1-003（原第 3 项）WTSBaseDataMgr::getTplIDByPID 对不含 "." 的 pid 越界访问 ay[1]
- 状态: 待验证（非法 PID 的接口输入合同未确认）
- 位置: `src/WTSTools/WTSBaseDataMgr.cpp` L880-888（`StrUtil::split(pid, ".")` 后直接 `ay[1]`）
- 触发: 公开入口 `isHoliday("NOPIVOT", date, false)`、`setTradingDate`/`getTradingDate`（非模板 pid 无点）等
- 影响: 对少于两段的 `pid`，`ay[1]` 超出 split 结果范围，形成未定义行为；是否崩溃不确定。当前接口未写明 `pid` 是否必须是 `exchange.product` 格式，因此此候选只证明 malformed input 的防御缺口，不证明有效 PID 会触发。
- 用例: `r1_storage/r1_basedatamgr_test.cpp` 的 `DISABLED_getTplIDByPID_single_segment_pid_is_oob`。本轮启用运行后 rc=0、`EXPECT_FALSE` 通过；没有 sanitizer，故未动态检出越界，原文“会崩”不成立为本次证据。
- 建议: `ay.size() < 2` 时返回 ""
- 复核裁定（2026-10-29）: 源码越界访问成立，但触发值 `NOPIVOT` 是单段 malformed PID，现有 API/文档未证明它属于允许输入；本机用例通过也不能证明 UB 安全。按输入验证/健壮性缺口待定，不登记为有效业务输入下的已证产品故障。修复方向：`ay.size() < 2` 时返回空模板 ID（让公开 `isHoliday` 等入口按未匹配路径返回），并为 malformed PID 保留不崩溃断言；确认 PID 格式约束后再定级。与 R18 的另一个解析函数只是同类输入边界，不是同一代码缺陷。

## BUG-R1-004（原第 4 项）WTSBaseDataMgr::loadSessions 在 `sections` 缺失/非数组时泄漏已创建的 WTSSessionInfo
- 状态: 已合并（与 BUG-Q05-005 的 loadSessions 子项同一代码路径）
- 位置: `src/WTSTools/WTSBaseDataMgr.cpp` L265 创建 `sInfo`，L283-284 `continue` 未入 `_session_map`，也无 release
- 触发: 配置里 session 无 `sections` 键（如 BADSEC）
- 影响: 每处坏配置泄漏一个 session 对象（量小，但静默）
- 复核裁定（2026-10-29）: 泄漏路径由源码确认：`WTSSessionInfo::create` 初始引用计数为 1，缺 `sections` 时 `continue` 前不释放也未交给 map；正常 `sessions_loaded_and_missing_sections_skipped` 用例通过只确认 BADSEC 被跳过，不检查泄漏。此项与 `q05_dtsrv.md` BUG-Q05-005 对同一 `loadSessions` 行为的记录重复，建议以 Q05-005 为 canonical。修复方向：用 RAII 持有新建对象，成功 `add` 到 session map 后再转交；无效 sections 分支释放对象。

## BUG-R1-005（原第 5 项）WtDataReader::readKlineSlice 无历史数据时 `&barsList._bars[0]` 取下标 0 于空 vector
- 状态: 静态越界风险成立；动态 sanitizer 未运行
- 位置: `src/WtDataStorage/WtDataReader.cpp` L1906-1912（else 尾窗分支 `head = &barsList._bars[size - hisCnt]`，hisCnt=0、vector 空）
- 触发: `readKlineSlice` 查询从未落盘的代码（用例 `missing_his_file_returns_empty_slice_not_null` 实测当前 Release 配置不崩）
- 影响: 在空 `_bars` 上执行 `&barsList._bars[barsList._bars.size() - hisCnt]`，此路径 `hisCnt==0`，即评估 `vector::operator[](0)`；空 vector 的下标 0 越过有效范围，属标准层面的 UB。随后 `appendBlock(head, 0)` 会因 count 为 0 早退，因此当前 Release 用例可返回空切片，不能据此声称已观察到崩溃。debug STL 检测未运行。
- 建议: hisCnt==0 时跳过取址
- 复核裁定（2026-10-29）: 条件性低影响产品缺陷（空历史文件路径上的越界下标表达式）；`missing_his_file_returns_empty_slice_not_null` 本轮通过并只确认空切片结果，不能覆盖 UB。修复方向是在构造 `head` 前检查 `hisCnt > 0`，与前一分支 L1900-1904 的守卫保持一致。与 P09 的零条数/one-past 指针问题同类，但代码路径不同，不作为重复条目。

---

## BUG-R1-006 历史日线按数量取尾窗时未遵守截止日期

- 状态: 本地隔离测试复现；其它周期及真实数据源未验证
- 严重级别: Major
- 位置: `src/WtDataStorage/WtDataReader.cpp:1760-1785,1906-1914`；接口截止时间说明见 `src/Includes/IDataReader.h:262-271`。
- 相关测试: `R1ReaderBarsTest.day_his_cutoff_excludes_later_bar`；旧 tail、旧结构升级及缓存命中用例原先用 1 月 8 日截止却期望 1 月 9/10 日数据，现改为截止 1 月 11 日以匹配其正向测试目的。
- 输入/前置条件: 本地历史日线文件包含 2024-01-08 和 2024-01-10，当前交易日为 1 月 10 日；请求 count=2、etime=2024-01-09 15:00，截止严格位于两条日期之间。
- 期望行为: 切片只包含截止之前的 1 月 8 日日线，不应包含 1 月 10 日数据。
- 实际行为: 定向测试 FAIL，返回 2 条且有一条日期为 20240110。非当日历史分支根据 `count` 从整个缓存末尾取切片，没有按 `endTDate` 定位截止位置。
- 修复方向: 对按日期排序的历史 K 线先根据截止日期或周期时间确定最后可见位置，再从该位置向前截取至多 count 条；处理无数据、截止早于首条、等于边界和晚于末条的情形，避免空向量取址。为日线及分钟线分别验证边界，明确交易日与自然日期口径；不要仅针对测试中的日期特判。
- 证据边界: 本次只在 sandbox 中运行普通历史日线读取，修正后的 3 条尾窗/格式/缓存对照 PASS；分钟线、复权、实时拼接及真实数据源未运行。与 AD reader 的 BUG-R122-002 属不同实现路径，不将动态证据混为一项。

## 留给主控裁定：需要接缝才能进的类（本轮留白，未硬闯）

### A. WtDataWriter 主体（前缀/落盘/滚动/procTick 分支）——需要接缝
- `IDataWriter`/`WtDataWriter::init(params, sink)` 无条件起 proc/check 线程（`src/WtDataStorage/WtDataWriter.cpp` L119 起，L583-620 任务循环），本波纪律禁止线程/时序；
- 非线程可达的 procTick/pipeToTicks/getTickBlock 等全部是 **private**（`WtDataWriter.h` L238-726），且禁止 `#define private public`；p10 先例正是用宏进的，本轮不可复用。
- 结论：R1 本轮 **未对 WtDataWriter.cpp 新增用例**；需要主控提供「同步写入」测试接缝（如 asyncproc=false 时不起线程并允许同步 execute 队列，或提供 friend 测试钩子）。

### B. WtDataReader / WtRdmDtReader 的主力连续（HOT）与股票复权拼接分支——需要更富的替身
- `WtRdmDtReader.cpp` L1162-1362（hot 分支）、L1364 起（股票复权分支）要求 `getContract` 返回带 `_ruletag/_exright` 的真实 `WTSContractInfo` 且 `splitCustomSections` 返回非空 HotSections；q03 夹具 BaseDataMgr 的 `getContract` 恒 NULL。可扩夹具，但 HotSection 因子语义大，留主控决定投入产出。

### C. getRT*Block 的「容量变化重映射」分支——需要接缝
- `WtDataReader.cpp` L1920-2130 与 `WtRdmDtReader.cpp` 对应函数的 `_last_cap != _block->_capacity` 重映射路径，依赖 BoostMappingFile 进程级映射与文件大小中途变化；单测里同 key 二次 map 的行为与真实 writer 侧扩容量时序耦合，禁 sleep/禁时序纪律下留白。
