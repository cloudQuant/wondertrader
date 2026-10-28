# BUG-R23-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtDataStorage/WtDataWriter.cpp`（`init` L168、`check_loop` L1616-1621、`release` L177-215、`~WtDataWriter` L103-105）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_writer_tick_cache_test.cpp` → `R23WriterTest.DISABLED_SecondInitOnSameWriterTerminatesProcess`
- 输入/前置条件: 同一个 `WtDataWriter` 对象上调用两次 `init(params, sink)`（公开虚函数，签名与 `IDataWriter` 一致，没有任何「只能调一次」的守卫或文档说明）。
- 期望行为: 重复 init 要么幂等地复用/先停止已有的检查线程，要么明确返回 false 并给出可诊断日志。
- 实际行为: L168 无条件 `_proc_chk.reset(new StdThread(boost::bind(&WtDataWriter::check_loop, this)))`。`StdThreadPtr = std::shared_ptr<std::thread>`，`reset()` 会让上一个 `std::thread` 引用归零；该线程此刻正睡在 L1621 的 `sleep_for(seconds(10))` 里、仍然 joinable → `~std::thread` 按标准直接 `std::terminate()`，进程当场终止（不是抛异常，无法 catch）。
- 复现步骤: 已实测复现 —— `/tmp/.../CoverageTests --gtest_also_run_disabled_tests --gtest_filter=R23WriterTest.DISABLED_SecondInitOnSameWriterTerminatesProcess` 退出码 **134（SIGABRT）**，stderr 只有一行 `libc++abi: terminating`，停在第 4 条语句（第二次 `init`）之后。
- 影响范围: 目前生产链路只有 `WtDtServo` 调一次 init，所以线上不易命中；但任何「改配置热重启落地器」「测试/工具复用同一对象」的写法都是硬终止。连带问题：`release()` 只 join `_proc_thrd`，**从不 join 也不置空 `_proc_chk`**，`~WtDataWriter()` 又是空的 —— 也就是说只要调过 `init()`，正常 `delete writer` 同样会走到 `~std::thread(joinable)` → `std::terminate`。本车道所有走 init 的用例因此「只 release、不 delete」（详见 manifest 的线程纪律一节）。
- 备注: 修复方向：`init` 开头先 `_terminated=true` + 通知并 join 旧的 `_proc_chk` 再重建；或把 `_proc_chk` 设为 detached 并在 release 里按标志收尾。

# BUG-R23-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtDataWriter.cpp`（`writeTransaction` L524-527，另见 L148-150、L2492）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_writer_kline_hft_test.cpp` → `R23WriterTest.DisableOrdDtlAlsoBlocksTransaction`
- 输入/前置条件: `init` 参数里 `disableorddtl=true`（`disabletrans` 保持 false），随后调用 `writeTransaction(trans)`；另一组只设 `disabletrans=true`。
- 期望行为: 三个逐笔写入入口各自只看自己的开关 —— `writeTransaction` 应在 `disabletrans=true` 时返回 false、在 `disableorddtl=true` 时照常放行。
- 实际行为: L526 写的是 `if (curTrans == NULL || _disable_orddtl) return false;`，判的是**委托明细**的开关（与 L472 的 `writeOrderDetail` 同一份变量）。同时 `_disable_trans` 在整条写入路径上没有任何读者，只在 `proc_loop` 的收盘作业 L2492 被读一次。实测：`disableorddtl=true` 时 `writeTransaction` 返回 false 且 `rt/trans/<exchg>/<code>.dmb` 不生成；`disabletrans=true` 时 `writeTransaction` 返回 true 且正常落块。
- 复现步骤: 直接运行该用例（两组子用例一起看）。
- 影响范围: 只想关委托明细的数据源会连带把逐笔成交也关掉（静默丢数据）；想单独关逐笔成交的配置则完全不生效。
- 备注: 该用例固化的是**当前行为**，修好后期望值要同步改成 `EXPECT_TRUE(...)`，用例注释里已标出。属于 copy-paste 型缺陷，一行改动。

# BUG-R23-003

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp`（`readTickSliceByCount` L2398-2401）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_rdm_count_slice_test.cpp` → `R23RdmCountTest.DISABLED_CountByCodeShapedPidDereferencesNullCommodity`
- 输入/前置条件: 宿主挂的是真实 `WTSBaseDataMgr`（其 `getCommodity(exchg, pid)` 按 `"exchg.product"` 建表查，见 `src/WTSTools/WTSBaseDataMgr.cpp` L81-89），调用 `readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000)`。本车道用一个与真实查表语义一致的 `StrictBDMgr` 复现。
- 期望行为: 与同文件另外 20+ 处一致，取时段应使用已经拿到的 `commInfo`（L2398 的结果）或 `getCommodity(_exchg, _product)`，并对 NULL 做判定。
- 实际行为: L2401 `_base_data_mgr->getSession(_base_data_mgr->getCommodity(cInfo._exchg, cInfo._code)->getSession())` —— 传的是**合约代码** `600000` 而不是 product id，查表键 `SSE.600000` 必然落空返回 NULL，紧接着无条件 `->getSession()` → 段错误。这是 `WtRdmDtReader.cpp`/`WtDataReader.cpp` 里唯一一处用 `_code` 查品种的 getCommodity。
- 复现步骤: 已实测复现 —— `--gtest_also_run_disabled_tests --gtest_filter=R23RdmCountTest.DISABLED_CountByCodeShapedPidDereferencesNullCommodity` 退出码 **139（SIGSEGV）**，无 gtest 失败输出，直接死在 L2401。
- 影响范围: `readTickSliceByCount` 在真实宿主里一调即崩；本车道的启用用例之所以不炸，是因为沿用了 q03 夹具里「恒返回非空」的 `getCommodity` 替身。策略侧 `stra_get_ticks(code, count)` 这类按条数取数的公开接口应该都走不到正常返回。
- 备注: 未证明的部分：没有验证过线上是否已有别的兜底路径把这条挡住（`WtDtMgr`/`WtEngine` 的调用面不在本车道范围）。修复方向：改成 `commInfo->getSession()` 并对 `commInfo`/`sInfo` 各加一次 NULL 判定。

# BUG-R23-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp`（`readTickSliceByCount` L2536-2543、`_his_tick_map` 声明 L195）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_rdm_count_slice_test.cpp` → `R23RdmCountTest.CountSizeCheckFailureLeavesEmptyEntryForever`
- 输入/前置条件: `his/ticks/SSE/20240108/600000.dsb` 存在但只有 8 字节（小于 `sizeof(HisTickBlock)`），前一日 20240107 文件正常；连续两次 `readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000)`。
- 期望行为: 校验失败时应把这条**不完整的缓存条目撤掉**（或不写入缓存），下次调用重新尝试读文件、并再次给出错误日志。
- 实际行为: L2536 先 `HisTBlockPair& tBlkPair = _his_tick_map[key];` 无条件把默认构造好的空条目插进 map，L2538 判长度失败只 `tBlkPair._buffer.clear(); break;`，`_block` 保持 NULL 而条目留在表里。第二次同样请求时 `bHasHisTick = (it != _his_tick_map.end())` 直接为真 → 不再读文件、不再打日志，进入 `while(bHasHisTick)` 后因 `_block == NULL` 立刻 break → 该交易日被**永久静默跳过**。实测：第一次 `LL_ERROR` 计数从 0 变 1、切片 3 条；第二次 `LL_ERROR` 计数仍为 1、切片仍 3 条（少了 20240108 那 3 条）。
- 复现步骤: 运行该用例（两次调用都在里面，断言已经写出计数差）。
- 影响范围: 一个坏掉的历史 tick 文件会让该交易日在进程生命周期内消失，且第二次以后连错误日志都没有 —— 上层看到的只是「那天没数据」。同一写法在 `readTickSliceByDate/ByRange` 里也存在（本车道未展开逐条验证，只确认 ByCount 这条）。
- 备注: 与 BUG-Q03-005（`clearCache()` 不回收 `_his_tick_map` 缓冲）是不同问题：那条讲内存单调增长，这条讲失败条目污染缓存。

# BUG-R23-005

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp`（RT 分支 L2449-2466、his 分支 L2577-2592）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_rdm_count_slice_test.cpp` → `RtWindowBoundaryKeepsTickExactlyAtEtime` / `HisWindowBoundaryDropsTickExactlyAtEtime`
- 输入/前置条件: 同一份 3 条 tick（14:30/14:31/14:32），分别放进当日 RT 块 `rt/ticks/SSE/600000.dmb` 与历史文件 `his/ticks/SSE/<date>/600000.dsb`，`etime` 的时分秒正好压在 14:31:00.000 那条上。
- 期望行为: 同一个「取到 etime 为止的最后 N 条」语义，RT 与 his 两条路径的右边界应当一致（含或不含边界二选一）。
- 实际行为: RT 分支回退条件是 `pTick->action_date > eTick.action_date || pTick->action_time > eTick.action_time`（L2459），边界那条**保留** → 拿到 14:30、14:31 两条；his 分支写的是 `pTick->action_time >= eTick.action_time`（L2585），边界那条**被退掉** → 只拿到 14:30 一条，剩下的缺口由更前面的交易日补。
- 复现步骤: 两个用例一起跑即可对比（断言已写成具体条数与具体时间戳）。
- 影响范围: 跨「当日/历史」边界的取数会少一条，且条数由数据落在哪条分支决定；回放与实时查询同一 etime 的结果不一致。
- 备注: 未证明成因（是刻意的「历史不重复取边界」还是 copy-paste 差异），只登记可复现的输出差别。

# BUG-R23-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtDataWriter.cpp`（`updateCache` L1455-1459、`resizeRTBlock` L313-364）
- 相关测试: 本车道未能构造触发输入（见 manifest 留白面「需要可控失败的 remap」）
- 输入/前置条件: 逻辑前提：`resizeRTBlock` 有两条返回 NULL 的路径（L336-340 追加写抛异常、L347-351 重新 map 失败），而 `updateCache` 在扩容后无条件使用返回值。
- 期望行为: 扩容失败时应保留旧映射（或记录后放弃本条 tick），不能把 `_tick_cache_block` 置空后继续解引用。
- 实际行为: L1457 `_tick_cache_block = (RTTickCache*)resizeRTBlock<RTTickCache, TickCacheItem>(...)` 的返回值直接被下一行 L1458 的 `_tick_cache_block->_capacity` 解引用；而 resizeRTBlock 在 remap 之前已经 `mfPtr.reset()`（L343）把旧映射放掉了，所以返回 NULL 时既崩溃又无回退路径。
- 复现步骤: 尚未复现（需要让 `BoostMappingFile::map` 在扩容时刻失败，例如把文件换成不可映射的对象；本车道预算内没有构造出来）。
- 影响范围: 磁盘满 / 并发 truncate cache.dmb 之类的异常场景下，落地器从「可恢复的扩容失败」变成「进程崩溃且缓存文件已被截断」。
- 备注: 只登记代码事实（返回值两条 NULL 路径 + 下一行无条件解引用），因果未经运行验证，请评审按源码判定。同型写法还出现在 `pipeToTicks` L637-639（那里**有** `if(blk)` 守卫）与 `pipeToKlines` L1096-1098、L1203-1205（**没有**守卫，随后 L1101 起直接 `blk->_size`），三处里只有 tick 分支做了保护。

# BUG-R23-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp`（`indexBarFromCacheByCount` L1694-1696、`readKlineSliceByCount` L2376-2392）、`src/Includes/WTSDataDef.hpp`（`WTSKlineSlice::create` L240-246）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_rdm_count_slice_test.cpp` → `R23RdmCountTest.KlineCountWithNoBarsAnywhereReturnsPhantomSlice`
- 输入/前置条件: `readKlineSliceByCount("SSE.STK.600000", KP_Minute5, 5, 202401101500)`，该 code 既没有 `his/min5/SSE/600000.dsb` 也没有 `rt/min5/SSE/600000.dmb`（或有一个 barcnt==0 的空文件）。
- 期望行为: 没有任何数据时返回 NULL（本函数 L2392 确实写着「`hisCnt + rtCnt > 0` 才建切片」），或者至少返回 `size()==0` 的空切片。
- 实际行为: `indexBarFromCacheByCount` 在 `barsList._bars.empty()` 时 `return NULL`，但**没有把引用出参 `count` 归零**；调用方 L2378-2379 传的正是 `hisCnt`，于是 `hisCnt` 保持「请求条数」5 → L2384 判定通过 → `WTSKlineSlice::create(stdCode, period, 1, NULL, 5)`：`create` 里 `if(bars)` 不挂块，却无条件 `_count = count`。结果得到一个 `size()==5`、`get_block_counts()==0`、`at(0)==NULL` 的「幻影切片」。实测断言：`size()==5` / `get_block_counts()==0` / `at(0)==NULL` / `at(-1)==NULL`；换成 barcnt==0 的空文件同样（`size()==3`）。
- 复现步骤: 直接运行该用例。
- 影响范围: 任何 `for(uint32_t i=0;i<slice->size();i++) slice->at(i)` 形式的调用（策略侧 `stra_get_bars` 一类）会在查询一个不存在的合约/周期时拿到 NULL 元素并解引用；而 `if(slice == NULL)` 这种防御写法反而全部失效。同型的「出参 count 不被清空」在 `readBarsFromCacheByRange` 侧已由 BUG-P09-002 登记，这条是它在 ByCount 入口上的具体后果（可观测到 size 与块数不一致）。
- 备注: 修复方向二选一 —— `indexBarFromCacheByCount` 空缓存分支里 `count = 0` 后再 return NULL；或 `readKlineSliceByCount` 用返回值 NULL 作为「无数据」判定而不是用 `hisCnt+rtCnt`。
