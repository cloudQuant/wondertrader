# Lane R5 交付清单 — WtDtCore/StateMonitor.cpp + WtDtCore/UDPCaster.cpp（迭代01 第七波）

- 派生自: `manifests/api-brief-R5.md`（路 5，`docs/迭代计划/迭代01/reports/分派范围-20261027T113454Z.md`）
- 落点目录: `src/TestUnits/coverage/q05_dtsrv/`（已登记目录，前缀 `r5_`；未新建目录、未改 `src/TestUnits/coverage/CMakeLists.txt`）
- 生产代码改动: **无**。`src/**`（除本 lane 两个新 .cpp）一字未动。
- 门禁（真实构建口径，非自造 flags）:
  `cd build-cov && ninja -t commands TestUnits/coverage/CMakeFiles/CoverageTests_dtcore.dir/q05_dtsrv/q05_state_monitor_test.cpp.o | tail -1`
  → 去掉 `-o X -c Y` 后对两个新 .cpp 加 `-fsyntax-only` 执行；**2/2 通过**（该目标带 `-Wall -Wextra -Werror -fprofile-instr-generate -fcoverage-mapping`，零告警）。
- 用例总数 23：**启用 21**，`DISABLED_` 2。不起线程、不 sleep、不建 socket、不 bind、不 join、不等条件变量。
- 引用面：只用 `WtDtCore` 自己的 `StateMonitor`/`UDPCaster`/`DataManager`（前向声明传 nullptr），未触碰与 `WtCore` 重名的 `wtp::ParserAdapter/ParserAdapterMgr/WtHelper`（BUG-L00-005）；本 lane 不需要 `ParserAdapter.h`，故未复现 `boost::noncopyable` 缺 include 问题（BUG-L00-004 已登记）。

## 1. `src/TestUnits/coverage/q05_dtsrv/r5_udp_caster_test.cpp`（251 行 / 12 例，11 启用 + 1 DISABLED）

| 用例（行号=测试文件内） | 生产文件:行段 | 打到的分支 |
| --- | --- | --- |
| `CtorYieldsSelfConsistentEmptyState` L36 | `src/WtDtCore/UDPCaster.cpp:49-60`、`:288-291` | ctor 三标量初始化 + dtor；`m_sktBroadcast==NULL` 守卫（用 `retainCount()==1` 证伪「被接管入队」） |
| `AllFourBroadcastOverloadsAreNoopWithoutBroadcastSocket` L50 | `:268-286`、`:288-291` | 4 个 `broadcast` 重载转发；tick/ordQue/ordDtl/trans 四个 `UDP_MSG_PUSH*` 常量入口都走同一守卫（`:22-26`） |
| `BroadcastOfNullObjectIsRejectedByGuard` L79 | `:288-291` | 守卫的 `data == NULL` 子条件（4 个重载各喂一次 nullptr） |
| `StopBeforeStartIsSafeAndTerminatesCaster` L95 | `:133-143` | `stop()` 未 start 时 `m_thrdIO`/`m_thrdCast` 两个判空 false 分支；`m_bTerminated=true` 后 broadcast 走第三守卫；重复 stop 幂等 |
| `AddBRecverAcceptsValidIPv4IntoTypeBucket` L113 | `:222-234` | 合法 IPv4；`type==0/1/2` 三个桶（L228-233）+ 未知 type 落入「无 else、仍然 return true」（BUG-R5-005） |
| `AddBRecverRejectsUnparsableAddressViaCatch` L128 | `:226-227`、`:235-240` | `make_address_v4` 抛出 → `catch(...) return false`：空串/主机名/256.1.1.1/三段/IPv6 两种写法 |
| `AddBRecverAcceptsPortBoundariesAndDuplicates` L143 | `:222-234`、`:288-291` | port=0/65535/-1 静默截断（BUG-R5-006）；重复登记不去重；登记后仍无 socket → 广播照样被拦 |
| `RegisteredReceiversAloneNeverEnableBroadcastPath` L161 | `:290`、`:325`（反向证据） | 列表非空但 `m_sktBroadcast==NULL`，`do_broadcast` 在组包前返回，证明 L300-395 循环只在 `start()` 之后才可达 |
| `BaseClassPointerDispatchesToUdpBroadcastNoop` L178 | `:268-286`、`src/WtDtCore/IDataCaster.h:34-52` | 基类指针虚表分派到 4 个 override（含 3 个接口默认实现在 tick/ordQue 上的覆盖关系） |
| `AddMRecverRejectsBadAddressBeforeAnySocket` L197 | `:246-248`、`:260-265` | 组播侧同样先解析地址后建 socket，非法地址在 `new UDPSocket`（L250）**之前**返回 false，全程不 bind |
| `DISABLED_AddMRecverWithValidAddressNeedsRealSocket` L210 | `:249-259` | 触发条件：`new UDPSocket(ioservice, endpoint(v4, sendport))` + `join_group` 即真 bind 端口，违反 lane 约束，故禁用占位 |
| `InitReturnsFalseWhenInactiveAndTouchesNothing` L216 | `:62-68` | `init()` 的 `active` false 早退（显式 false 与缺省 false 各一次），`broadcast`/`multicast` 数组与 `start()` 均不进入；配置经 `WTSCfgLoader::load_from_file`（`load_from_json` 是 private，已核实） |

本文件**未覆盖**（写清以免下波重复踩）：`:70-96`（active=true 后的接收方装配 + sport/bport 回落）、`:101-131`（start）、`:145-220`（do_receive 全解析）、`:249-259`（合法组播）、`:293-400`（do_broadcast 锁/队列/组包/send_to）、`:402-416`（死代码，BUG-R5-004）。理由与接缝需求见 `bugs/R5_wtdtcore.md` BUG-R5-001/-002/-003 与文末「需要接缝/环境」清单。

## 2. `src/TestUnits/coverage/q05_dtsrv/r5_state_monitor_bdt_test.cpp`（358 行 / 11 例，10 启用 + 1 DISABLED）

与既有 `q05_state_monitor_test.cpp`（6 例）不重叠：那份只有 `loadSessions`，`getSessionComms` 恒 NULL，因此 `StateMonitor.cpp:109-134` 的品种/节假日循环整块未走；本文件补的就是这段 + 换算/入表的边角。

| 用例 | 生产文件:行段 | 打到的分支 |
| --- | --- | --- |
| `SessionCommsPresentOverwritesSeededTradingDate` L149 | `src/WtDtCore/StateMonitor.cpp:109-133`（L110 if 真、L119-123 setTradingDate、L128 条件） | 日盘 SSE(2 品种) + 夜盘 SHFE(1 品种，`getOffsetMins()<=0`) 的品种循环；用 `setTradingDate` 预置哨兵 20200101/20200102 后断言被覆写 |
| `SessionWithoutCommsKeepsSeededTradingDate` L176 | `:61-66`（模板不存在 continue）、`:109-110`（对照组） | 只配 15 字符会话时长它的品种循环跑（独立模板 LONG_H），同时断言未被配置的 SSE_H 哨兵纹丝不动；再喂 NOSUCH 会话验证 `getSession()==NULL` 分支不碰交易日 |
| `TodayAsHolidayDrivesHolidayLogBranchOfInitLoop` L203 | `:112-115`、`:124-132`（L131 的 `Instrument {} is in holiday`） | 运行时把 `TimeUtils::getCurDate()` 与前一天写进 holidays.json，使 `isTradingDate(pid, offDate)` 为假 → 走 info 分支；initialize 仍 true |
| `FifteenCharSessionIdFitsStateKeyBuffer` L221 | `:74`（`strcpy` 进 `StateMonitor.h:47` 的 `char[16]`）、`:107` | 15 字符会话名恰好写满缓冲（键完整、截断键不命中）；16+ 字符溢出仅登记为 BUG-R5-010，不写启用用例 |
| `MissingTimeFieldsDefaultToZeroAndStillAssembleSections` L238 | `:70-72`（`getUInt32` 的 defVal=0 路径）、`:76-105` | `{"SSE": {}}` 与 `{"SHFE": {"inittime":2045}}` 两个残缺条目仍完成集合竞价/交易区间换算并入表 |
| `ReinitializeWithExtraSessionGrowsStateTable` L253 | `:107`（`_map[key]` 覆写与追加）、`:57-59` | 两次 initialize 叠加：同键覆写 + 新键追加；`isAllInState(SS_CLOSED)` 在混合表上为 false |
| `SessionWithoutAuctionKeepsOnlyTradingSections` L271 | `:76-88`（循环 0 次）、`:90-105` | 无 `auction/auctions` 的会话模板：`getAuctionSections()` 为空仍只装配交易区间；`getSessionComms("NOAUC")==NULL` |
| `ZeroStartSectionStillAssemblesWithoutValidation` L286 | `:90-105`（L96-100 `stime--` 下溢）、`:70-72` | 三个时间字段全 0 且 `sections.from=0` 的畸形配置：initialize 不校验、照旧入表返回 true（BUG-R5-009 的现状锁定） |
| `NonObjectEntriesDegradeToDefaultTimes` L300 | `:56-59`、`:68-72`、`:107` | 会话项是数组/字符串时 `WTSVariant::get` 返回 NULL（`Includes/WTSVariant.hpp:364-374`），三个时间取 defVal；顶层是数组时 `memberNames()` 空、循环 0 次仍返回 true |
| `MixedKnownAndUnknownSessionsKeepTableConsistent` L317 | `:61-66`（多次 continue）+ `:109-133` | 3 个未知模板夹 2 个已知模板：已知会话入表且品种循环跑到（哨兵被覆写），未知会话 `isInState` 恒 false |
| `DISABLED_StateMachineAdvanceNeedsInjectableClockSeam` L346 | `:145-530`、`:537-543` | 触发条件：`run()` 即 `new StdThread` + `sleep_for(2ms)` 轮询墙上时钟，`stop()` join；无注入时钟可把 `curMin` 定在目标 HHMM，且 `lastTime` 是函数级 `static` 跨用例污染 → 整条禁用，详见 BUG-R5-002 |

头文件内联面（`StateMonitor.h:70-81 isInSections`、`:169-222` 三个查询）随上述用例一并被驱动，归因落在 `StateMonitor.h`，不计入本 .cpp 行数。

## 3. 预期增量与不可达上界

- `StateMonitor.cpp`：本轮把可静态装配的 `initialize`（L38-137）打到「无死分支」程度；剩余 ~400 行属于 `run()` 线程 lambda（L145-530）与 `stop()`（L537-543），在无时钟接缝、不起线程的口径下**不可达**，占该文件 missed 的绝大多数（BUG-R5-002）。
- `UDPCaster.cpp`：本轮把 ctor/dtor、`init` 的 active 守卫、`stop` 的空线程守卫、`addBRecver` 全分支、`addMRecver` 的 socket 前分支、4 个 `broadcast` + `do_broadcast` 前置守卫打满（约 60-70 行）；`start`/`do_receive`/`addMRecver` 合法分支/`do_broadcast` 工作线程体（L101-131、L145-220、L249-259、L293-400，约 300 行）要求真 socket + io_context 线程，`L402-416` 是死代码（BUG-R5-001/-003/-004 及文末清单）。
- 结论：这两个文件的覆盖率上界受「无接缝」硬约束压制，不是用例数量问题；若主控允许（a）可注入时钟/单次 `checkOnce`，（b）`UDPSocket` 工厂接缝或 `handle_packet(buf,len)` 内存入口，则 `StateMonitor.cpp` 可再吃 ~400 行、`UDPCaster.cpp` 可再吃 ~150 行。
