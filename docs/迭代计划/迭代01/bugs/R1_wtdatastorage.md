# R1 缺陷清单 — WtDataStorage / WTSDataDef / WTSBaseDataMgr

发现者：lane R1（覆盖率第七波）。所有条目均**未修改产品代码**，只记录。

---

## 1. WTSTickSlice::_count 在 create(code) 单参路径下未初始化（读未初始化内存）
- 位置: `src/Includes/WTSDataDef.hpp` L1398-1418（构造器 `WTSTickSlice() { _blocks.clear(); }` 不初始化 `_count`；`create` 仅在 `ticks != NULL` 时赋值 `_count`）
- 触发: `WTSTickSlice::create("x")` 后调用 `size()` / `empty()` / `at(0)`
- 影响: UB；Release 下大概率 0，ASan/MSan 直接报；WtDataReader/Rdm 的空结果路径都从这条 create 起步
- 建议: 构造器补 `_count = 0`
- 状态: 待审核

## 2. WtRdmDtReader 的逐笔委托/成交历史缓存查错 map（缓存永不命中 + 顺序相关的假 NULL）
- 位置: `src/WtDataStorage/WtRdmDtReader.cpp`
  - readOrdDtlSliceByRange his 分支 L876：`_his_ordque_map.find(key)`，但装载进 `_his_orddtl_map[key]`（L884、L913）
  - readTransSliceByRange his 分支 L1051：同样查 `_his_ordque_map`，装载进 `_his_trans_map`（L1059、L1088）
- 触发/后果:
  1) dtl/trans 缓存**永远不会命中**（find 的 map 与 add 的 map 不同），每次调用重复读盘解压；
  2) 若同一 `stdCode-endTDate` key 曾缓存过 queue（`_his_ordque_map` 命中），则 dtl/trans 跳过装载块，落到 `tBlkPair._block == NULL → return NULL`（L915-916、L1090-1091），**即使 .dsb 文件存在也返回空**。
- 复现: 先 readOrdQueSliceByRange 再 readOrdDtlSliceByRange（同 code 同日期）即可观测 2)
- 建议: find 改为各自的 `_his_orddtl_map` / `_his_trans_map`
- 状态: 待审核（R1 用例只测「首次读」，未把该缺陷行为固化成断言）

## 3. WTSBaseDataMgr::getTplIDByPID 对不含 "." 的 pid 越界访问 ay[1]
- 位置: `src/WTSTools/WTSBaseDataMgr.cpp` L880-888（`StrUtil::split(pid, ".")` 后直接 `ay[1]`）
- 触发: 公开入口 `isHoliday("NOPIVOT", date, false)`、`setTradingDate`/`getTradingDate`（非模板 pid 无点）等
- 影响: `operator[]` 越界 = UB/崩溃
- 用例: `r1_storage/r1_basedatamgr_test.cpp` 中 `DISABLED_getTplIDByPID_single_segment_pid_is_oob`（`--gtest_also_run_disabled_tests` 会崩，进程级，不可断言存活）
- 建议: `ay.size() < 2` 时返回 ""
- 状态: 待审核

## 4. WTSBaseDataMgr::loadSessions 在 `sections` 缺失/非数组时泄漏已创建的 WTSSessionInfo
- 位置: `src/WTSTools/WTSBaseDataMgr.cpp` L265 创建 `sInfo`，L283-284 `continue` 未入 `_session_map`，也无 release
- 触发: 配置里 session 无 `sections` 键（如 BADSEC）
- 影响: 每处坏配置泄漏一个 session 对象（量小，但静默）
- 状态: 待审核

## 5. WtDataReader::readKlineSlice 无历史数据时 `&barsList._bars[0]` 取下标 0 于空 vector
- 位置: `src/WtDataStorage/WtDataReader.cpp` L1906-1912（else 尾窗分支 `head = &barsList._bars[size - hisCnt]`，hisCnt=0、vector 空）
- 触发: `readKlineSlice` 查询从未落盘的代码（用例 `missing_his_file_returns_empty_slice_not_null` 实测当前 Release 配置不崩）
- 影响: 理论 UB（`&v[0]` on empty）；libc++ debug/容器检查开启即断言
- 建议: hisCnt==0 时跳过取址
- 状态: 待审核

---

## 留给主控裁定：需要接缝才能进的类（本轮留白，未硬闯）

### A. WtDataWriter 主体（前缀/落盘/滚动/procTick 分支）——需要接缝
- `IDataWriter`/`WtDataWriter::init(params, sink)` 无条件起 proc/check 线程（`src/WtDataStorage/WtDataWriter.cpp` L119 起，L583-620 任务循环），本波纪律禁止线程/时序；
- 非线程可达的 procTick/pipeToTicks/getTickBlock 等全部是 **private**（`WtDataWriter.h` L238-726），且禁止 `#define private public`；p10 先例正是用宏进的，本轮不可复用。
- 结论：R1 本轮 **未对 WtDataWriter.cpp 新增用例**；需要主控提供「同步写入」测试接缝（如 asyncproc=false 时不起线程并允许同步 execute 队列，或提供 friend 测试钩子）。

### B. WtDataReader / WtRdmDtReader 的主力连续（HOT）与股票复权拼接分支——需要更富的替身
- `WtRdmDtReader.cpp` L1162-1362（hot 分支）、L1364 起（股票复权分支）要求 `getContract` 返回带 `_ruletag/_exright` 的真实 `WTSContractInfo` 且 `splitCustomSections` 返回非空 HotSections；q03 夹具 BaseDataMgr 的 `getContract` 恒 NULL。可扩夹具，但 HotSection 因子语义大，留主控决定投入产出。

### C. getRT*Block 的「容量变化重映射」分支——需要接缝
- `WtDataReader.cpp` L1920-2130 与 `WtRdmDtReader.cpp` 对应函数的 `_last_cap != _block->_capacity` 重映射路径，依赖 BoostMappingFile 进程级映射与文件大小中途变化；单测里同 key 二次 map 的行为与真实 writer 侧扩容量时序耦合，禁 sleep/禁时序纪律下留白。
