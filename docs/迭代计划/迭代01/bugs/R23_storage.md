# BUG-R23-001

- 状态: 正常释放的线程生命周期缺陷由源码确认；重复 init 合同待确认
- 严重级别: Blocker
- 模块/文件: `src/WtDataStorage/WtDataWriter.cpp`（`init` L168、`check_loop` L1616-1621、`release` L177-215、`~WtDataWriter` L103-105）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_writer_tick_cache_test.cpp` → `R23WriterTest.DISABLED_SecondInitOnSameWriterTerminatesProcess`
- 输入/前置条件: 正常 init 后 release 并销毁对象；另有同一个对象重复调用 `init(params, sink)` 的条件场景，接口未规定是否支持重入。
- 期望行为: 正常释放和销毁不应留下访问对象的线程；重复 init 的语义需先明确，若不支持应拒绝重复调用。
- 实际行为: L168 无条件 `_proc_chk.reset(new StdThread(boost::bind(&WtDataWriter::check_loop, this)))`。`StdThreadPtr = std::shared_ptr<std::thread>`，`reset()` 让旧 `std::thread` 对象析构；只要旧线程仍是 joinable（无论仍在运行还是已结束但未 join/detach），`~std::thread` 都会调用 `std::terminate()`，不是可捕获异常。
- 复现步骤: 已实测复现 —— `/tmp/.../CoverageTests --gtest_also_run_disabled_tests --gtest_filter=R23WriterTest.DISABLED_SecondInitOnSameWriterTerminatesProcess` 退出码 **134（SIGABRT）**，stderr 只有一行 `libc++abi: terminating`，停在第 4 条语句（第二次 `init`）之后。
- 影响范围: 重复 init 会硬终止。全生产调用面是否只 init 一次未在本次审查验证；此外，`release()` 只 join `_proc_thrd`，**从不 join `_proc_chk`**，而 `~WtDataWriter()` 为空，因此 init 后正常销毁也存在 joinable 线程析构终止风险。本车道用例只 release、不 delete（详见 manifest 的线程纪律一节）。
- 复核（2026-10-29）: 源码确认 `init` 启动捕获 `this` 的 `_proc_chk`，`release` 只 join `_proc_thrd`，检查线程还会访问块映射；所以不仅是第二次 init 的 joinable 线程析构问题，释放资源前也必须等待检查线程退出。
- 复核判定（2026-10-29）: 正常生命周期的线程清理缺陷由源码支持：调用方 release 后删除 writer，而检查线程未 join；正常销毁与并发退出尚未单独动态复现。历史重复 init 用例以 134/SIGABRT 退出，但重复 init 是否属于有效合同尚待明确。
- 备注: 修复方向：明确 init/release 状态机，拒绝重复 init 或先停止并 join 旧线程；用线程安全的终止信号和可唤醒等待替代无条件 10 秒 sleep，`release`/析构在释放块之前确保检查线程退出，保证重复 release 安全，再补正常销毁、重复 init 和并发退出测试。不可仅 detach：线程捕获 `this`，对象或缓存释放后可能发生 use-after-free。

# BUG-R23-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtDataWriter.cpp`（`writeTransaction` L524-527，另见 L148-150、L2492）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_writer_kline_hft_test.cpp` → `R23WriterTest.DisableOrderDetailAndTransactionAreIndependent`
- 输入/前置条件: `init` 参数里 `disableorddtl=true`（`disabletrans` 保持 false），随后调用 `writeTransaction(trans)`；另一组只设 `disabletrans=true`。
- 期望行为: 三个逐笔写入入口各自只看自己的开关 —— `writeTransaction` 应在 `disabletrans=true` 时返回 false、在 `disableorddtl=true` 时照常放行。
- 实际行为: L526 写的是 `if (curTrans == NULL || _disable_orddtl) return false;`，判的是**委托明细**的开关（与 L472 的 `writeOrderDetail` 同一份变量）。同时 `_disable_trans` 在整条写入路径上没有任何读者，只在 `proc_loop` 的收盘作业 L2492 被读一次。实测：`disableorddtl=true` 时 `writeTransaction` 返回 false 且 `rt/trans/<exchg>/<code>.dmb` 不生成；`disabletrans=true` 时 `writeTransaction` 返回 true 且正常落块。
- 复现步骤: 直接运行该用例（两组子用例一起看）。
- 影响范围: 只想关委托明细的数据源会连带把逐笔成交也关掉（静默丢数据）；想单独关逐笔成交的配置则完全不生效。
- 复核判定（2026-10-29）: 产品缺陷。旧用例通过只表示它记录了现状；已改为双向验证两个开关独立生效，重新构建后单跑 FAIL。`disabletrans` 的独立禁用语义由参数名、其他逐笔入口的分开开关及测试注释支持；未找到单独的外部配置规范。最小修复是将 `writeTransaction` 的判断改为 `_disable_trans`，并以该测试回归。

# BUG-R23-003

- 状态: 源码缺陷确认；最终严格夹具候选未运行
- 严重级别: Blocker
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp`（`readTickSliceByCount` L2398-2401）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_rdm_count_slice_test.cpp` → `R23RdmCountTest.DISABLED_CountByCodeShapedPidDereferencesNullCommodity`
- 输入/前置条件: 宿主挂的是真实 `WTSBaseDataMgr`（其 `getCommodity(exchg, pid)` 按 `"exchg.product"` 建表查，见 `src/WTSTools/WTSBaseDataMgr.cpp` L81-89），调用 `readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000)`。本车道用一个与真实查表语义一致的 `StrictBDMgr` 复现。
- 期望行为: 与同文件另外 20+ 处一致，取时段应使用已经拿到的 `commInfo`（L2398 的结果）或 `getCommodity(_exchg, _product)`，并对 NULL 做判定。
- 实际行为: L2401 `_base_data_mgr->getSession(_base_data_mgr->getCommodity(cInfo._exchg, cInfo._code)->getSession())` —— 传的是**合约代码** `600000` 而不是 product id，查表键 `SSE.600000` 必然落空返回 NULL，紧接着无条件 `->getSession()` → 段错误。这是 `WtRdmDtReader.cpp`/`WtDataReader.cpp` 里唯一一处用 `_code` 查品种的 getCommodity。
- 复现步骤: 历史版本禁用用例曾以 139（SIGSEGV）退出，未取得崩溃栈，不能将信号独立定位到特定指令。现在已改为严格品种键与有效 RT 数据的候选，保持禁用，仅重建编译，尚未执行最终版本。
- 影响范围: 对该 code 形态、真实 `WTSBaseDataMgr` 查表语义且能到达此 reader 方法的调用会在 `getSession()` 前空指针解引用；本车道启用用例因沿用 q03 夹具里「恒返回非空」的 `getCommodity` 替身而不炸。策略侧 `stra_get_ticks(code, count)` 的公开调用链是否到达此路径未验证。
- 备注: 未证明的部分：没有验证过线上是否已有别的兜底路径把这条挡住（`WtDtMgr`/`WtEngine` 的调用面不在本车道范围）。修复方向：改成 `commInfo->getSession()` 并对 `commInfo`/`sInfo` 各加一次 NULL 判定。
- 复核判定（2026-10-29）: 产品缺陷源码路径明确；真实 `WTSBaseDataMgr` 的 `"exchg.product"` 键语义与严格测试替身一致。最终候选未运行，未检查宿主上层所有调用面是否另有拦截。

# BUG-R23-004

- 状态: 本地文件修复后重试回归 FAIL；其它查询分支未动态验证
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp`（`readTickSliceByCount` L2536-2543、`_his_tick_map` 声明 L195）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_rdm_count_slice_test.cpp` → `R23RdmCountTest.CountSizeCheckFailureCanRecoverAfterFileRepair`
- 输入/前置条件: `his/ticks/SSE/20240108/600000.dsb` 首次只有 8 字节，前一日文件正常；首次读取后修复 20240108 文件，再用同一 reader 按相同截止时间查询。
- 期望行为: 校验失败时应把这条**不完整的缓存条目撤掉**（或不写入缓存）；文件修复后下次查询应重新读取修复日数据。
- 实际行为: L2536 先 `HisTBlockPair& tBlkPair = _his_tick_map[key];` 无条件把默认构造好的空条目插进 map，L2538 判长度失败只 `tBlkPair._buffer.clear(); break;`，`_block` 保持 NULL 而条目留在表里。第二次同样请求时 `bHasHisTick = (it != _his_tick_map.end())` 直接为真 → 不再读文件、不再打日志，进入 `while(bHasHisTick)` 后因 `_block == NULL` 立刻 break → 该交易日被**永久静默跳过**。实测：第一次 `LL_ERROR` 计数从 0 变 1、切片 3 条；第二次 `LL_ERROR` 计数仍为 1、切片仍 3 条（少了 20240108 那 3 条）。
- 复现步骤: 运行该用例（两次调用都在里面，断言已经写出计数差）。
- 影响范围: 一个坏掉的历史 tick 文件会让该交易日在进程生命周期内消失，且第二次以后连错误日志都没有 —— 上层看到的只是「那天没数据」。同一写法在 `readTickSliceByDate/ByRange` 里也存在（本车道未展开逐条验证，只确认 ByCount 这条）。
- 备注: 与 BUG-Q03-005（`clearCache()` 不回收 `_his_tick_map` 缓冲）是不同问题：那条讲内存单调增长，这条讲失败条目污染缓存。
- 复核判定（2026-10-29）: 产品缺陷；原现状式断言已改为 `CountSizeCheckFailureCanRecoverAfterFileRepair`，先修复 20240108 文件，再期望同一 reader 读回该日。定向运行 FAIL：实际首尾仍是 20240107 的价格 330/332，而非修复日的 130/132。结论仅覆盖已运行的 ByCount 测试；ByDate/ByRange 是相似静态模式，本轮未分别执行。修复方向是失败时清理缓存条目或仅在校验成功后插入，并回归文件修复后的重试。
- 新回归（2026-10-29）: 将上述固化现状的旧测试改为修复文件后用同一 reader 重新查询；单跑 FAIL，返回的仍为 20240107 而非已修复的 20240108 数据。

# BUG-R23-005

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp`（RT 分支 L2449-2466、his 分支 L2577-2592）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_rdm_count_slice_test.cpp` → `RtWindowBoundaryKeepsTickExactlyAtEtime` / `HisWindowBoundaryDropsTickExactlyAtEtime`
- 输入/前置条件: 同一份 3 条 tick（14:30/14:31/14:32），分别放进当日 RT 块 `rt/ticks/SSE/600000.dmb` 与历史文件 `his/ticks/SSE/<date>/600000.dsb`，`etime` 的时分秒正好压在 14:31:00.000 那条上。
- 期望行为: 边界契约尚未从接口文档或调用方证实；若 RT 与历史共用同一「取到 etime 为止的最后 N 条」语义，则两条路径应一致含/不含边界。先确认契约再定预期。
- 实际行为: RT 分支回退条件是 `pTick->action_date > eTick.action_date || pTick->action_time > eTick.action_time`（L2459），边界那条**保留** → 拿到 14:30、14:31 两条；his 分支写的是 `pTick->action_time >= eTick.action_time`（L2585），边界那条**被退掉** → 只拿到 14:30 一条，剩下的缺口由更前面的交易日补。
- 复现步骤: 本轮分别运行 `HisWindowBoundaryDropsTickExactlyAtEtime` 与 `RtWindowBoundaryKeepsTickExactlyAtEtime`，两个用例均通过；这确认了当前两条路径分别返回的时间戳，不证明哪种边界语义是接口要求。
- 影响范围: 跨「当日/历史」边界的取数会少一条，且条数由数据落在哪条分支决定；回放与实时查询同一 etime 的结果不一致。
- 复核判定（2026-10-29）: 未定，暂不判产品缺陷，也不判测试错误；现有测试是行为观察，未证明期望契约。修复方向：先从 API/调用方确认 etime 是否闭区间，再用相同输入对 RT 与历史路径写契约测试。

# BUG-R23-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtDataWriter.cpp`（`updateCache` L1455-1459、`resizeRTBlock` L313-364）
- 相关测试: 本车道未能构造触发输入（见 manifest 留白面「需要可控失败的 remap」）
- 输入/前置条件: 逻辑前提：`resizeRTBlock` 有两条返回 NULL 的路径（L336-340 追加写抛异常、L347-351 重新 map 失败），而 `updateCache` 在扩容后无条件使用返回值。
- 期望行为: 扩容失败时应保留旧映射（或记录后放弃本条 tick），不能把 `_tick_cache_block` 置空后继续解引用。
- 实际行为: L1457 `_tick_cache_block = (RTTickCache*)resizeRTBlock<RTTickCache, TickCacheItem>(...)` 的返回值直接被下一行 L1458 的 `_tick_cache_block->_capacity` 解引用；而 resizeRTBlock 在 remap 之前已经 `mfPtr.reset()`（L343）把旧映射放掉了，所以返回 NULL 时既崩溃又无回退路径。
- 复现步骤: 尚未复现（需要让 `BoostMappingFile::map` 在扩容时刻失败，例如把文件换成不可映射的对象；本车道预算内没有构造出来）。
- 影响范围: 若扩容追加或重新映射失败，`updateCache` 会把 NULL 写回 `_tick_cache_block` 后立刻解引用；`procKline` 的 minute1/minute5 两个调用点也没有 NULL 守卫。扩容失败的真实触发条件、进程表现和文件完整性本轮均未动态验证；不能据此断言缓存文件已被截断。
- 备注: 只登记代码事实（返回值两条 NULL 路径 + 下一行无条件解引用），因果未经运行验证，请评审按源码判定。同型写法还出现在 `pipeToTicks` L637-639（那里**有** `if(blk)` 守卫）与 `pipeToKlines` L1096-1098、L1203-1205（**没有**守卫，随后 L1101 起直接 `blk->_size`），三处里只有 tick 分支做了保护。
- 复核判定（2026-10-29）: 静态源码确认的产品错误路径，尚未动态复现；无失败注入测试，因此保持“未复现”。建议扩容时先建立新映射、成功后再交换旧映射，并在所有调用点检查 NULL；补充追加失败与 remap 失败注入测试。

# BUG-R23-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp`（`indexBarFromCacheByCount` L1694-1696、`readKlineSliceByCount` L2376-2392）、`src/Includes/WTSDataDef.hpp`（`WTSKlineSlice::create` L240-246）
- 相关测试: `src/TestUnits/coverage/r23_storage4/r23_rdm_count_slice_test.cpp` → `R23RdmCountTest.KlineCountWithNoBarsAnywhereReturnsEmptyResult`
- 输入/前置条件: `readKlineSliceByCount("SSE.STK.600000", KP_Minute5, 5, 202401101500)`，该 code 既没有 `his/min5/SSE/600000.dsb` 也没有 `rt/min5/SSE/600000.dmb`（或有一个 barcnt==0 的空文件）。
- 期望行为: 没有任何数据时返回 NULL（本函数 L2392 确实写着「`hisCnt + rtCnt > 0` 才建切片」），或者至少返回 `size()==0` 的空切片。
- 实际行为: `indexBarFromCacheByCount` 在 `barsList._bars.empty()` 时 `return NULL`，但**没有把引用出参 `count` 归零**；调用方 L2378-2379 传的正是 `hisCnt`，于是 `hisCnt` 保持「请求条数」5 → L2384 判定通过 → `WTSKlineSlice::create(stdCode, period, 1, NULL, 5)`：`create` 里 `if(bars)` 不挂块，却无条件 `_count = count`。结果得到一个 `size()==5`、`get_block_counts()==0`、`at(0)==NULL` 的「幻影切片」。实测断言：`size()==5` / `get_block_counts()==0` / `at(0)==NULL` / `at(-1)==NULL`；换成 barcnt==0 的空文件同样（`size()==3`）。
- 复现步骤: 直接运行该用例。
- 影响范围: 调用方可能把 `size()` 当作可读元素数，但 `at(i)` 对该切片返回 NULL；是否有公开策略调用方因此解引用并崩溃，本轮未验证。与 BUG-P09-002 都涉及空历史缓存，但根因和路径不同：P09-002 是 ByRange 拷贝路径的空 vector 下标/长度下溢，本条是 ByCount 的 `count` 出参未清零导致幻影切片，不是重复项。
- 备注: 修复方向二选一 —— `indexBarFromCacheByCount` 空缓存分支里 `count = 0` 后再 return NULL；或 `readKlineSliceByCount` 用返回值 NULL 作为「无数据」判定而不是用 `hisCnt+rtCnt`。
- 复核判定（2026-10-29）: 产品缺陷；`KlineCountWithNoBarsAnywhereReturnsPhantomSlice` 运行通过，断言确认 `size()>0`、无 block 且 `at(0)==NULL`。这证明切片元数据与数据块不一致，但未证明消费端崩溃。
- 新回归（2026-10-29）: 已将旧测试改为断言无数据时返回 NULL 或零长度切片；重新构建后无文件和空文件两种输入均 FAIL，分别观察到非零 size。
