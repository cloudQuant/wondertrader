# Lane q05_dtsrv 交付清单 — WtDtCore / WtDtServo / WtDtHelper（迭代01 第五波）

- 工作流: q05（覆盖率迭代01 第五波，目标目录已登记 `CoverageTests_dtsrv`：`src/WtDtCore,WtDtServo,WtDtHelper|dir:q05_dtsrv`）
- 生产代码改动: 无。`src/**`（除 `src/TestUnits/coverage/q05_dtsrv/`）与所有 CMakeLists 一字未动。
- 简报依据: `manifests/api-brief-L05.md` + `manifests/l05_dt_servo_porter.md`（l05 点名跳过的 StateMonitor/DataManager/IndexWorkerMgr/ParserAdapter/resample_bars 即本波靶子）。

## 1. 新增测试文件（`src/TestUnits/coverage/q05_dtsrv/`，共 5 个，启用用例 28 条 + DISABLED 1 条）

| 文件 | 规模 | 用例 |
| --- | --- | --- |
| `q05_state_monitor_test.cpp` | ~185 行 / 6 例 | StateMonitor::initialize 全分支 + 三个状态查询 |
| `q05_dtcore_datamgr_test.cpp` | ~200 行 / 4 例 | WtDtCore::DataManager init 失败路径 + IDataWriterSink 全部委托 + 广播扇出 |
| `q05_idxworker_mgr_test.cpp` | ~290 行 / 7 例 | IndexWorkerMgr loadFactories/init/handle_quote/context 委托 + IdxWorkerWrapper 生命周期 |
| `q05_servo_wtdatamgr_test.cpp` | ~150 行 / 3+1 例 | WtDtServo::WtDataManager 空实例守卫 + init(无 store) + 1 条 DISABLED 崩溃收口 |
| `q05_resample_bars_test.cpp` | ~300 行 / 8 例 | WtDtHelper::resample_bars 全部分支（l05 遗留 240 行的主体） |

落盘全部走 `std::filesystem::temp_directory_path()/q05_<tag>_<pid>_<seq>` 唯一目录，TearDown `remove_all`；不起线程、不 join、不等条件变量、不占 socket/shm、不加载插件 DLL。

## 2. 覆盖的符号（llvm-cov 归因目标）

WtDtCore（模块实测 0.7%，本波主攻）
- `StateMonitor::initialize`（cpp L38-137：文件缺失/坏 JSON/空对象/未知会话 continue/日盘+夜盘会话装配/`getOffsetDate` 前的 `_session_to_pids==NULL` 跳过分支）、`isAnyInState/isAllInState/isInState`（头文件内联）
- `DataManager`：ctor/dtor、`init`（load_library 失败 return false、`_bd_mgr/_state_mon` 先行赋值语义）、`release/add_ext_dumper/writeTick/writeOrderQueue/writeOrderDetail/writeTransaction/getCurTick/isSessionProceeded/transHisData` 的 `_writer==NULL` 守卫、`getBDMgr/canSessionReceive(全天候+状态机双分支)/broadcastTick/broadcastOrdQue/broadcastOrdDtl/broadcastTrans/getSessionComms/getTradingDate/outputLog`、`add_caster` 内联
- `IndexWorkerMgr`：ctor/dtor、`loadFactories`（目录缺失 false、空目录 true、子目录/非 .dylib 跳过；不加载真模块，L62-91 的 dlopen 分支未覆盖）、`init`（无 indice false / active=false / 名称无点号 / 工厂缺失 continue / poolsize=0 不建池）、`handle_quote`（NULL/未订阅/已订阅同步分支）、`sub_ticks/get_tick/push_tick/output_log/get_hot_mgr/get_bd_mgr`；`IdxWorkerWrapper::self`/析构
- `IDataCaster` 默认空实现分支经 `DataManager::broadcast*` 扇出再次覆盖

WtDtServo
- `WtDataManager`：ctor/dtor、`init`（runner==NULL + store 缺失安全 false）、`initStore` 的 cfg==NULL 早退（经 init 间接，注意：**简报 L77 的 initStore 实为 private**，无法直测其余分支）、`update_bars(_rt_bars==NULL)`、`clear_subbed_bars`、`get_exright_factor(commInfo==NULL)`、`reader_log`、`clear_cache(_reader==NULL)`

WtDtHelper（剩余 240 行主体）
- `resample_bars`（cpp L833-1034）：m1/m5/d/h/f/非法周期分派、日/分钟时间格式校验（含 `fromTime>=1e8||endTime>1e8` 的 >=/> 不对称）、fromTime>endTime 交换、sessInfo 解析失败/缺 sections、文件缺失与头部过短、lower_bound 未命中「没有找到指定时间范围的K线」、日线全区间透传、m1 全区间自一致性往返（store_bars→resample_bars）

## 3. 跳过目标及原因（排除清单候选）

- `WtDtCore/UDPCaster.cpp/.h` 全部 — boost::asio UDP 广播/组播 socket；按任务令点名跳过，不用真实端口。
- `WtDtCore/ShmCaster.cpp/.h` 全部 — boost::interprocess 命名共享内存段。
- `StateMonitor::run/stop` — StdThread 监控线程 + 秒级 sleep 轮询（禁止阻塞路径）。
- `IndexWorkerMgr::loadFactories` 的 dlopen 分支（L62-91）与 `createWorker` 的工厂命中/`_workers` 注册分发 — 需真实指标插件 DLL；`_factories/_workers` 为私有且无注入点。
- `IndexWorkerMgr::init` 的 `poolsize>0` 分支 — `boost::threadpool::pool` 构造即起服务线程（禁止线程）。
- **两套 `ParserAdapter/ParserAdapterMgr`（WtDtCore + WtDtServo）全部方法** — 同名同签名强符号在 `CoverageTests_dtsrv` 二进制内必 duplicate（见 BUG-Q05-002，**该目标当前链接可行性存疑，主控须先处置**）；任务令里「用替身 IParserSink 驱动 ParserAdapter 过滤/时间戳判定」的子目标因此整体移交主控拆分后补波，本波一行未碰。
- `WtDtServo` 其余（`WtDtRunner::proc_tick/sub_tick/sub_bar/trigger_*`、`WtDataManager::subscribe_bar/get_*_slice`）— 依赖真实 reader/runner 全量初始化，`subscribe_bar` 无 reader 即空指针（归入 BUG-Q05-001 同类）。
- `DataManager::init` 的成功尾段（`_writer->init`）与 `transHisData` 实转 — 需 `createWriter` 插件。

## 4. 门禁结果

命令（仓库根，逐文件，与任务令一致）：`clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY -include src/Share/fmt_enum_compat.h -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include <file>`

结果：**5/5 文件 0 error 0 warning**。期间修正两处：`WTSBarStruct` 无 `code/exchg` 成员（删字段填充）；`WtDataManager::initStore` 为 private（改经 `init` 间接覆盖）。未运行 cmake/ninja/测试二进制。

## 5. 待审核缺陷

`docs/迭代计划/迭代01/bugs/q05_dtsrv.md`：BUG-Q05-001 ~ BUG-Q05-005（2 Blocker / 1 Major / 2 Minor）+ 3 条「待裁定」。崩溃类输入全部收进唯一 `DISABLED_` 用例，启用用例零残留。

## 6. 需要主控接手的事项

1. 先解决 BUG-Q05-002（两套 ParserAdapter 同目标链接），否则 `CoverageTests_dtsrv` 可能根本链不出来，本波及后续所有 dtsrv 归因失效；建议按目录加编译别名拆分或移出其中一份 TU。
2. `q05_resample_bars_test.cpp` 的两处计数断言按 bugs 文档「待裁定」处理。
3. 若需要一个能跑 ParserAdapter 的目标，请另开 lane 并先给类加命名空间。

## 7. 预算

工具调用：45 次（≤60，第 6 次前完成首个 Write）。
