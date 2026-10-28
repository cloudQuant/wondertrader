# Lane R18 交付清单 — WtDtCore（ParserAdapter / IndexWorkerMgr / ShmCaster / DataManager / StateMonitor.h）

- 派生自: `manifests/api-brief-R18.md`；先读过 `manifests/R5_wtdtcore.md`、`bugs/R5_wtdtcore.md`、`manifests/q05_dtsrv.md` 再动手。
- 落点目录: `src/TestUnits/coverage/q05_dtsrv/`（已登记目录，前缀 `r18_`；未新建目录、未改任何 `CMakeLists.txt` —— 目标 `CoverageTests_dtcore` 的 `dir:q05_dtsrv` 由 `src/TestUnits/coverage/CMakeLists.txt` 的 `GLOB` 自动纳入新文件）。
- 生产代码改动: **无**。`src/**`（除本 lane 四个新 .cpp）一字未动。
- 引用面: 全部只走 `WtDtCore` 一族（`ParserAdapter/ParserAdapterMgr/DataManager/IndexWorkerMgr/StateMonitor/ShmCaster/IDataCaster/WtHelper` + `WTSTools/WTSBaseDataMgr`），未 include 任何 `WtCore`/`WtDtServo` 头文件，避免同名 `wtp::ParserAdapter/ParserAdapterMgr/WtHelper` 的 duplicate/静默择一（BUG-L00-005）。
- 硬约束: 无 `#define private public`；不起线程、不 sleep、不建 socket、不建共享内存段/映射文件；断言不读挂钟（全部是本用例自喂输入算得的期望值：指针身份、引用计数、尺寸/偏移、分支返回值）；链接完整性自担 —— 所有调用都走该 TU 真正导出的公开成员（`ParserAdapter::*`、`ParserAdapterMgr::*`、`IndexWorkerMgr::{loadFactories,init,handle_quote,sub_ticks,get_tick,push_tick,output_log,get_hot_mgr,get_bd_mgr}`、`ShmCaster::{init,broadcast}`、`DataManager::{init,canSessionReceive,getBDMgr,writeTick,getCurTick,release}`、`StateMonitor` 的三个公开查询与头文件公开结构体 `StateInfo`），没有任何 extern 重声明。
- 用例总数 36：**启用 28**，`DISABLED_` 8。
- 门禁（真实构建口径 flags，非自造）: `cd build-cov && ninja -t commands TestUnits/coverage/CMakeFiles/CoverageTests_dtcore.dir/q05_dtsrv/q05_state_monitor_test.cpp.o | tail -1` → 剥掉 `-MD/-MT/-MF/-o X -c Y` 后对四个新 .cpp 逐一 `-fsyntax-only`（该目标带 `-Wall -Wextra -Werror -fprofile-instr-generate -fcoverage-mapping`）：**4/4 PASS，0 error 0 warning**（脚本 `build-cov/r18_gate.sh` 留存）。未执行构建、未运行二进制、未跑 ctest。

## 1. `src/TestUnits/coverage/q05_dtsrv/r18_parser_adapter_test.cpp`（11 启用 + 3 DISABLED）

生产文件 `src/WtDtCore/ParserAdapter.cpp`（373 行，报表 249/254 missed、现 1.97%）与 `src/WtDtCore/ParserAdapter.h`（94 行）。

| 用例 | 生产文件:行段 | 打到的分支 |
| --- | --- | --- |
| `FreshAdapterIsIdleAndExposesBaseDataMgr` | `ParserAdapter.cpp:30-44`、`:207-219`、`:221-228`、`:230-233`、`:311-322`；`ParserAdapter.h:42`、`:88` | 构造标量初始化；`run()` 的 `_parser_api==NULL` false 分支；`handleSymbolList` 空实现；`handleParserLog` 的未停止（`:316 log_raw_by_cat`）与已停止（`:313-314`）两侧；`release()` 在「无 api + `_remover==NULL`」下走 `:218 delete NULL` 且不崩（二次 release 同样安全）；`getBaseDataMgr`；`id()`/`size()` 内联 |
| `InitExtStillReturnsTrueWhenApiInitFails` | `:46-58`、`:74-80`、`:207-219` | `api!=NULL` 后的 `registerSpi(this)`；`init(NULL)` 失败只打日志仍 `return true`；`release()` 里 `_remover==NULL → delete`（替身 `*_destroyed` 标志证明 delete 真发生） |
| `InitExtSubscribesEveryContractFullCode` | `:54-73`（`:60-69` 合约循环、`:66` `getFullCode()`） | brief 尾段：遍历 `WTSArray` 拼 `ContractSet` 并 `subscribe`；用恰好 2 个合约的自喂配置断言集合恰为 `{SSE.600000, SHFE.rb2410}`，并断言不含 altcode `SSE.SH600000` |
| `InitRejectsNullCfgEmptyModuleAndMissingModule` | `:84-120`（`:86-87` cfg NULL、`:89` 先写 `_id`、`:91-92` 重复守卫、`:94-95` retain、`:99-100` 空 module、`:102-109` 回落 `get_module_dir()+"parsers/"`、`:111-116` 加载失败） | `init` 的四条拒绝；二次 init 证明 `_cfg` 已非空（BUG-R18-012 的外在表现） |
| `StoppedAdapterNeverFillsContractInfo` | `:286-309`（`:288-289` `_stopped` 守卫 vs `:294-298` 命中回写） | 同一 tick 停止前后各喂一次：未停止 `getContractInfo()==_ctSse`，`release()` 后仍 NULL → 证明守卫先于 `_bd_mgr` 查询 |
| `HandleQuoteRejectedWhenDatesZeroOrContractUnknown` | `:291-292`（`actiondate()==0`/`tradingdate()==0`）、`:294-302`（查不到合约） | 三种拒绝形状；用例把 `_dt_mgr` 置 NULL —— 漏守卫即 `:304` 空指针崩，「跑完 + 合约仍 NULL」即等价于证明走到了 return |
| `HandleQuoteKeepsPresetContractInsteadOfLookingUp` | `:294-299`（`if (contract==NULL)` 的 false 分支）、`:301-305` | 预置 `_ctShfe` 而 code 指向 SSE.600000 → 不被覆写；另一路证明命中查询后 `writeTick` 返回 false 时在 `:305` 早退（`_idx_fact` 传 nullptr 也不崩，反向证据） |
| `ThreeLevel2HandlersRejectBeforeDataManager` | `:235-245`、`:252-262`、`:269-279` | 三个逐笔入口的 `actiondate/tradingdate==0`（6 个短路子条件各喂一次）与 `getContract()==NULL`；同样跑在 `_dt_mgr==NULL` 的适配器上 |
| `ThreeLevel2HandlersResolveContractAndDelegateWrite` | `:247-249`、`:264-266`、`:281-283` | 三处 `setContractInfo` 副作用可观测（等于 `_bdMgr.getContract` 的同一指针），尾部 `writeXxx` 在 `_writer==NULL` 下静默返回 |
| `MgrAddAndGetGuards` | `:337-352`（`:339` 的 `adapter==NULL` 与 `strlen(id)==0` 两子条件、`:342-347` 重名）、`:354-363` | 空指针/空串 id/重名三类拒绝；`getAdapter` 命中与未命中（`:362` 空 ptr）；`_adapters` 是 public，直接断言 `size()` |
| `MgrRunAndReleaseDriveEveryAdapter` | `:327-335`、`:365-373`（含 `:372` 计数日志）、配合 `:221-228` | 两适配器表上 `run()` 各触发一次 `connect`；`release()` 逐个释放并 `clear`；表空后二次 `release()` 循环 0 次（不二次 delete） |
| `DISABLED_HandleQuoteCrashesWhenDataManagerIsNull` | `:304` | 触发条件：`_dt_mgr==NULL` 且日期齐全、合约命中 → 空指针解引用（BUG-R18-003） |
| `DISABLED_DoubleReleaseDoubleDelete` | `:207-219` | 触发条件：`initExt` 后连续两次 `release()` → 同一 `IParserApi` 被 `delete` 两次（BUG-R18-004） |
| `DISABLED_AddAdapterWithNullIdCrashesInStrlen` | `:339` | 触发条件：adapter 非空且 `id==NULL` → `strlen(NULL)`（BUG-R18-005） |

## 2. `src/TestUnits/coverage/q05_dtsrv/r18_idxworker_mgr_test.cpp`（5 启用 + 2 DISABLED）

生产文件 `src/WtDtCore/IndexWorkerMgr.cpp`（226 行，报表 missed 67）与 `IndexWorkerMgr.h`（95 行）。

| 用例 | 生产文件:行段 | 打到的分支 |
| --- | --- | --- |
| `LoadFactoriesSkipsDirectoryAndNonLibraryAndUnloadableFiles` | `:35-64`（`:37-41` 目录缺失、`:46-49` 子目录、`:54-56` 非 .dylib、`:62-64` 扩展名对但 `load_library` 失败 → `continue`）、`:95-97` | 扫描侧四类跳过；名字带 `.dylib` 的**子目录**证明 `is_directory` 早于扩展名判断；两个垃圾 .dylib 证明 dlopen 失败不抛不崩 |
| `LoadFactoriesSurvivesLibraryWithoutFactorySymbol` | `:62-71`（`hInst!=NULL` 后 `get_symbol("create_index_factory")==NULL` → `:69 free_library` → `:70 continue`）、`:95-97` | brief 点名的 `:69`；把 `/opt/homebrew/lib/*.dylib` 复制两个同内容样本各打一次；本机无样本时 `GTEST_SKIP`（不假绿不假红） |
| `InitAcceptsEmptyIndiceArrayButRejectsNonArrayIndice` | `:129-145`（`:135` poolsize=0 不建池；`:141-145` 的 `cfgIdx==NULL` 与 `!isArray()` 两个子条件）、`:147-167`（空数组循环 0 次 → `:167 return true`）、`:131-133` | brief 点名的「indice 是对象不是数组」拒绝一路 + 空数组放行一路；早退前三个上下文指针已写入，用 `get_hot_mgr/get_bd_mgr` 断言 |
| `SyncHandleQuoteKeepsOneExtraRetainPerQuote` | `:170-180`、`:182` 的 false 分支、`:195-202`（同步遍历 `_workers`，无 `release()`） | brief 点名的同步分支：自喂 create 的 tick 引用数 1 →（未订阅 SHFE.rb2410）1 →（已订阅两次）2 → 3；`handle_quote(nullptr)` 走 `:172-173` |
| `ContextDelegatesToDataManagerAndLogger` | `:205-208`（`push_tick → writeTick(…,1)`）、`:210-216`（合法两段式 fullCode 切分）、`:218-221`（`get_tick` 两种入参）、`:223-226`（brief 点名的 `:225 log_raw`） | 四个 `IIndexContext` 委托在 `_writer==NULL` 的 DataManager 上的返回值与不崩；`push_tick` 后引用数仍为 1（不接管所有权） |
| `DISABLED_HandleQuoteCrashesWhenContractInfoMissing` | `:173-175` | 触发条件：`newTick!=NULL` 但 `getContractInfo()==NULL` → `:175` 空指针解引用（BUG-R18-008） |
| `DISABLED_SubTicksWithoutDotCrashesOnIndex` | `:212-215` | 触发条件：`sub_ticks("600000")` 无点号 → `ay.size()==1` 时 `ay[1]` 越界（BUG-R18-009） |

本文件未覆盖：`:73-91`（工厂登记）、`:116-126`（`createWorker` 成功尾段，brief 的 `:124`）、`:136-139`+`:182-194`（线程池）。

## 3. `src/TestUnits/coverage/q05_dtsrv/r18_shm_caster_test.cpp`（7 启用 + 1 DISABLED）

生产文件 `src/WtDtCore/ShmCaster.cpp`（173 行，报表 58、0.00%）与 `ShmCaster.h`（145 行）。按任务令只做结构/尺寸/参数校验与拒绝分支，**未创建任何共享内存段或映射文件**。

| 用例 | 生产文件:行段 | 打到的分支 |
| --- | --- | --- |
| `DataItemLaysOutTypeWordThenUnionOfFourPayloads` | `ShmCaster.h:31-58`（`#pragma pack(push,8)` 的 `_DataItem`：`_type` + 匿名 union） | `sizeof(DataItem) == align_up(sizeof(uint32_t),8) + max(四个结构体尺寸)`（期望值由本用例自喂的 `WTSTickStruct/WTSOrdQueStruct/WTSOrdDtlStruct/WTSTransStruct` 算得）；四个 union 成员同址且等于 `_type` 之后 8 字节 |
| `QueueTemplateDefaultsCapacityReadableWritablePid` | `ShmCaster.h:65-84`（`:69` `_capacity = N` 默认成员初始化、`:83` 构造的 `_readable=UINT64_MAX/_writable=0/_pid=0`） | 本地模板实例 `_DataQueue<4>` 的四个标量初值 + `sizeof == 32 + 4*sizeof(DataItem)` |
| `RingIndexModuloWrapsWithinSmallQueue` | `ShmCaster.cpp:83-91`（照搬算式：`wIdx=_writable++`、`realIdx=wIdx % _capacity`、`_type` 赋值、`_readable=wIdx`） | 10 次写入的下标期望 0,1,2,3,0,1,2,3,0,1（本用例自喂的 i 直接算得）；锁定「第 i 次写后可读位是 i-1」与回绕覆盖语义（BUG-R18-011 的结构性证据） |
| `CastQueueIsTheFixed8192SlotInstantiation` | `ShmCaster.h:65`（默认模板实参 `8*1024`）、`:89`（`typedef _DataQueue<8*1024> CastQueue`） | `sizeof(CastQueue) == 32 + 8192*sizeof(DataItem)`，且等于 `_DataQueue<8*1024>` 与 `_DataQueue<>`；`static` 存储区上确认 `_capacity==8192`、`_readable==UINT64_MAX`（放 static 以免 3MB+ 栈溢出） |
| `InitRejectsNullConfig` | `ShmCaster.cpp:24-28` | `cfg==NULL` 拒绝 |
| `InitRejectsInactiveAndTouchesNoFile` | `ShmCaster.cpp:31-32`（显式 `false` 与缺省回落 `false` 各一次） | 两条拒绝都断言配置里的 `path` 文件**不存在** → 证明拒绝先于 `:38-43` 的任何 create/truncate |
| `FreshCasterRejectsAllFourBroadcastOverloads` | `ShmCaster.cpp:72-76`、`:99-103`、`:126-130`、`:153-157`；`ShmCaster.h:98`（`_queue=NULL,_inited=false`）、`:113-131`（四个 override） | 四个 `broadcast` 的第一子条件（数据为空）与第二子条件（`_queue==NULL`）各覆盖一遍；漏守卫即 `:83` 等处的空指针解引用 |
| `DISABLED_InitWithActiveTrueNeedsRealMappedFile` | `ShmCaster.cpp:35-64` | 触发条件：`active=true` 会 `create_or_open_file` + `truncate_file(sizeof(CastQueue))` + `BoostMappingFile::map` + 在映射地址上 `new CastQueue()` —— 真映射/共享内存面，任务令禁止；该面还缺校验（BUG-R18-010） |

## 4. `src/TestUnits/coverage/q05_dtsrv/r18_state_info_datamgr_test.cpp`（5 启用 + 2 DISABLED）

生产文件 `src/WtDtCore/StateMonitor.h`（232 行，报表 missed 49）与 `src/WtDtCore/DataManager.cpp`（324 行，报表 missed 33）。

| 用例 | 生产文件:行段 | 打到的分支 |
| --- | --- | --- |
| `DefaultStateInfoFieldsAreZeroedAndOriginal` | `StateMonitor.h:45-52`、`:87-95`（`_StateInfo()` 清零）、`:63` | 公开结构体 `StateInfo` 默认构造六项：`_session` 空串、三个时间 0、`_state==SS_ORIGINAL`、`_sInfo==nullptr`、`_sections` 空；再自喂两段区间供下条用 |
| `IsInSectionsUsesClosedOpenInterval` | `StateMonitor.h:70-81`（`:73` 循环、`:77` 的 `sec._from <= curTime && curTime < sec._end`、`:78 return true`、`:80 return false`） | brief 点名的区间判断：929 F / 930 T（下界闭）/ 1129 T / 1130 F（上界开）/ 1200 F（两轮不命中落到 `:80`）/ 1300 T（第二次迭代命中）/ 1499 T / 1500 F / 2000 F。R5 只能靠 `.cpp` 间接驱动，此处纯内存直喂 |
| `IsInSectionsOnEmptyAndDegenerateSections` | `StateMonitor.h:73`（0 次循环）、`:77`（`from==end` 与 `from>end` 两种畸形区间） | 空 `_sections` 恒 false；零宽区间 `[900,900]` 在 900/899 都 false；倒挂 `[1500,930]` 恒 false（现状锁定，配置不校验，呼应 BUG-R5-009） |
| `FreshMonitorQueriesOnEmptyStateTable` | `StateMonitor.h:169-182`、`:189-203`、`:211-222` | 空 `_map`：`isAnyInState` 0 次循环 → `:181 false`；`isAllInState` 0 次循环 → `:202 true`（vacuous）；`isInState` 未命中 → `:216-217 false`（含空串 sid）。R5 只覆盖过非空表 |
| `DataManagerInitFailsOnMissingModuleFileBothWays` | `DataManager.cpp:67-78`（`:69-70` 先赋 `_bd_mgr/_state_mon`；`:72-74` **module 缺省回落 WtDataStorage**；`:75-76` 显式名）、`:78`、`:100-104`（加载失败 error + `return false`）、`:127-134`、`:143-146`、`:200-206`、`:237-245`（`_state_mon==NULL` → 全天候 true） | q05 只打了显式名一路，本轮补 `:73-74` 默认名一路：先断言两个预测文件名（`libWtDataStorage.dylib` / `libr18_custom_store.dylib`）都不存在，再断言两次失败后临时目录条目数仍为 0（无任何落盘副作用）；`getBDMgr()` 在 init 失败后仍等于传入指针（`init` 不判 `bdMgr` 空） |
| `DISABLED_InitCrashesWhenParamsIsNull` | `DataManager.cpp:67-72` | 触发条件：`init(nullptr, ...)` → `params->getCString("module")` 空指针解引用（BUG-R18-001） |
| `DISABLED_InitCrashesWhenLoadedModuleHasNoWriterSymbols` | `:81-98`（brief 点名的 `:84`、`:90` 两条 error 与 `:93` 守卫不成立）、`:106`（无条件 `_writer->init`） | 触发条件：把任一真实 Mach-O dylib 复制成 `<module_dir>/libWtDataStorage.dylib` 后 init → 打到 `:84/:90/:93/:98`，`:106` 崩（BUG-R18-002）；本机无样本时自动跳过 |

## 5. 预期增量与留白（不含时钟/网络面，按任务令避让 R5 已登记的接缝面）

- `ParserAdapter.cpp`：可静态驱动的面（构造、`init` 四条拒绝、`initExt` 全部、`release`、`run`、五个 handle 的守卫与合约回写、`ParserAdapterMgr` 四个方法）全部打上，预计吃掉 249 missed 中约 150 行；余约 70 行卡在「必须有真实 parser 插件才进得去 `:122-137` 与 `:140-204`」以及 `:307-308`（`writeTick` 需真实存储写手才返回 true）。
- `IndexWorkerMgr.cpp`：`:62-71`、`:129-145`、`:147-167`、`:170-203` 同步侧、`:205-226` 全部打上；留白 `:73-91`、`:116-126`（真实指标工厂插件，且 `_factories/_workers` 私有无注入点）与 `:136-139`+`:182-194`（线程池，任务令禁线程），合计约 35 行。
- `ShmCaster.cpp/.h`：结构/尺寸/默认模板实参/环形下标算式/两条 init 拒绝/四个 broadcast 守卫 ≈ 12~18 行；`:35-64`（真映射尾段）与四个 `broadcast` 的写入体（`_queue` 私有且无 setter）≈ 40 行留白。
- `StateMonitor.h`：`:70-81` 与空表三个查询首次可独立直喂，约 12~16 行；其余仍属 R5 登记的 `run()` 时钟接缝面（BUG-R5-002），本 lane 未硬闯。
- `DataManager.cpp`：`:73-74` 默认模块名一路 + 无副作用断言 ≈ 3~5 行；`:81-98`、`:106` 的加载成功尾段只存在于崩溃路径（已 `DISABLED_` 并登记 BUG-R18-002），成功路径需真实存储写手，约 20 行留白。

## 6. 待审核缺陷

`docs/迭代计划/迭代01/bugs/R18_wtdtcore.md`：BUG-R18-001 ~ BUG-R18-012（2 Blocker / 7 Major / 3 Minor），每条含独立成行的 `- 状态: 待审核`，并逐条给出一行式复现（启用用例或 `DISABLED_` + 触发条件）。

## 7. 预算

工具调用约 62 次（超 55 次预算：前段花在 R5/q05 账本与五个生产文件的通读，以及 -fsyntax-only flags 的口径校正——`ninja -t commands` 的原命令含 `-MD -MT -MF`，直接剥 `-o/-c` 会把带引号的 `"-Wno-#pragma-messages"` 打成字面量，需 `eval` 保留引号）。
