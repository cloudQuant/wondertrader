# p10_storage_writer 交付清单 — WtDataStorage/WtDataWriter.cpp（第四波 lane p10）

- 目标文件：`src/WtDataStorage/WtDataWriter.cpp`（2701 行，迭代01 开工时 0.00%、2035 行未覆盖，全仓第二大单文件空白）
- 目标二进制：`CoverageTests`（`src/TestUnits/coverage/CMakeLists.txt` 的 `MAIN_PLUGIN_TUNS` 已编入 `src/WtDataStorage/*.cpp`，故可直接调用类内部方法）
- 生产代码改动：**无**。未改任何 `CMakeLists.txt`；未引用 `WtCore/WtUftCore/WtBtCore/WtDtCore` 的类；**未 include 任何 `WtDataStorageAD/*`**（避开 p05 实测的 `extern "C"` 同名工厂重复符号）
- 新增文件：`src/TestUnits/coverage/p10_storage_writer/` 下 3 个用例 .cpp + 1 个内部前置头
  - `p10_writer_prefix.h`（非用例；把 `Includes/FasterDefs.h` → `FasterLibs/ankerl/unordered_dense.h` 等第三方头挡在 `#define private public` 之外，见文件内注释）
  - `p10_day_dsb_dump_test.cpp` 12 个 `TEST_F`（含 0 个 DISABLED_）
  - `p10_proc_block_data_test.cpp` 12 个 `TEST_F`（11 启用 + 1 个 `DISABLED_` 崩溃类收口）
  - `p10_rt_block_files_test.cpp` 8 个 `TEST_F`
  - 合计 32 个用例（启用 31），断言宏 ≈ 260 条
- 落盘：全部在 `std::filesystem::temp_directory_path()/p10_day_<pid>_<seq>`、`p10_rt_<pid>_<seq>`，`TearDown` 先 `release()`+`delete` writer（回收 mmap），再 `remove_all(ec)`；**未读写 `dist/`**
- 无阻塞：不启动 `init()`/`release` 之外的任何线程，不 join、不 `sleep`、不 `condvar.wait`；`_async_proc` 保持默认 false，未调用 `pushTask/proc_loop/transHisData`

## 覆盖的函数/符号（符号名全部来自 api-brief-L04 或本轮 Read 实测行）

`WtDataWriter.cpp`
- `WtDataWriter::dump_day_data`（`:1909`）：新建块头（`BlockHeader`/`BLK_FLAG`/`BT_HIS_Day`/`BLOCK_VERSION_RAW_V2`）、追加、同日期不同值替换末条、同日期同值 no-op、旧日期忽略、`barcnt > 100` 切 `BlockHeaderV2`+`BLOCK_VERSION_CMP_V2`（并断言 `_size == filesize - 20`）、已压缩文件追加后仍压缩、`== 100` 条边界仍 RAW、altcode≠code 时 `rename` 老 `.dsb`（`setAltCode`）、不可写 `_base_dir` → 返回 false 且不落文件、只有头部的残骸文件（BUG-P10-004）
- `WtDataWriter::proc_block_data`（`:1818`）：RAW_V2 保头/去头、CMP_V2 解压（保头时头改写回 `BLOCK_VERSION_RAW_V2`、去头时得到逐条 bar）、CMP_V2 长度校验失败 → false 且内容零改写、`BLOCK_VERSION_RAW` 老 bar（`WTSBarStructOld`→`WTSBarStruct` 11 字段逐个升版比对）、老 tick（`WTSTickStructOld`→`WTSTickStruct`）、未知版本位透传、`_blk_flag` 不校验的实测行为、`BLOCK_VERSION_CMP` 走 V2 长度校验（BUG-P10-002）
- `WtDataWriter::dump_bars_to_file`（`:2020`）：`ct == NULL` → 返回 0
- `WtDataWriter::getTickBlock`（`:952`）：新建 `rt/ticks/<exchg>/<code>.dmb`（`sizeof(RTTickBlock) + 2500*sizeof(WTSTickStruct)`）、`_capacity/_size/_date/_type/_version/_blk_flag` 全字段、缓存表命中返回同一指针、**日期切换分支**（`:1009`，size 归零 + 数据 memset 清零，读回文件验证首条为 0）、**长度自愈分支**（`:1029-1047`，把文件截成 10 条 → `_capacity=10`、`_size=min(realCap, markedCap)=10`）、`bAutoCreate=false` 且文件缺失 → NULL
- `WtDataWriter::pipeToTicks`（`:624`）：写入 RT tick 文件块 → `_size` 递增 → `_file->sync()` 后按文件字节 **memcmp 整条 `WTSTickStruct` 逐字段读回比对**
- `WtDataWriter::getKlineBlock`（`:1311`）：`KP_Minute1`/`KP_Minute5` 两个 switch 分支、按 `getTradingMins()`（min5 再 `/5`）预分配容量、`rt/min1|min5/<exchg>/<code>.dmb` 的文件尺寸精确比对、缓存表命中、`default` 分支（非法周期 `(WTSKlinePeriod)99` → `cache_map == NULL` → NULL）、`ct == NULL` → NULL
- `WtDataWriter::getTransBlock`（`:857`）/`getOrdQueBlock`（`:668`）/`getOrdDtlBlock`（`:762`）：`rt/trans|queue|orders/<exchg>/<code>.dmb` 新建 + `_type`（`BT_RT_Trnsctn`/`BT_RT_OrdQueue`/`BT_RT_OrdDetail`）+ `_capacity == 2500` + `_size == 0` + `_date`、`ct == NULL`、`bAutoCreate=false`
- `WtDataWriter::releaseBlock`（`:1300`）与 `WtDataWriter::release`（`:177`，`_proc_thrd` 为空时只回收五张块表）、`WtDataWriter::isSessionProceeded`（`:107`，空表/未命中/过去日期 false/未来日期 true）
- `DataDefine.h`：`BlockHeader`/`BlockHeaderV2` 的 `is_old_version()`/`is_compressed()`（`:83/:91/:112/:120`）、`RTTickBlock`/`RTKlineBlock`/`RTDayBlockHeader` 布局、`BLK_FLAG`/`FLAG_SIZE`/`BlockType`/四个 `BLOCK_VERSION_*`
- `WTSCmpHelper::compress_data`（经 `proc_block_data` 解压路径闭环）、`WTSContractInfo::create/setAltCode/setCommInfo`、`WTSCommodityInfo::create/setSessionInfo/setCategory`、`WTSSessionInfo::create/addTradingSection/getTradingMins`、`WTSTickData::create(WTSTickStruct&)`、`WTSBarStruct`/`WTSBarStructOld`/`WTSTickStruct`/`WTSTickStructOld`

## 跳过项（可直接搬进排除清单；每条都是「需要 RT 共享内存/后台线程/会阻塞」或本轮预算外）

1. `WtDataWriter::init`（`WtDataWriter.cpp:117-176`）+ `WtDataWriter::release` 的线程部分：`init` 会 `new StdThread` 起 `proc_loop`/`check_loop`，`release` 需 `join` → 一条用例一个进程下必然拖长且有 HANG 风险。**本轮所有用例都不调 `init()`**，改为直接赋 `_base_dir`/`_save_tick_log` 私有成员驱动文件路径。
2. `WtDataWriter::check_loop`（`:1616`，含 `sleep_for(10s)` @ `:1621`）与清理线程（`:2385`，`sleep_for(300s)`）→ 直接撞 HANG 门禁，**整函数排除**。
3. `WtDataWriter::proc_loop`（`:2216`）、`pushTask`（`:578`）、`_task_cond/_task_mtx` 任务队列（`:583/:594/:601`）与 `writeTick/writeOrderQueue/writeOrderDetail/writeTransaction` 的 `_async_proc == true` 分支 → 依赖后台线程；`_async_task` 未初始化 + 无谓词 `wait` 的丢唤醒死锁已由 p05 记为 BUG-P05-005（本文件同型风险登记在 `bugs/p10_storage_writer.md` BUG-P10-005，未复现）→ **排除**。
4. `WtDataWriter::loadCache`（`:267`）、`updateCache`（`:1438`）、`getCurTick`（`:1418`）、`resizeRTBlock`（`:313`）与 `pipeToKlines`（`:1056`）：前者依赖 `_bd_mgr->getContractSize` + `cache.dmb` 的 `_tick_cache_block` 跨进程 RT 缓存表；`resizeRTBlock` 是 remap 扩容（`pipeToTicks` 容量写满才会触发，需要 2500+ 条 tick 才能自然进入，测试里用截断文件人为压小 capacity 会走进未读实现）→ 为守住「零崩溃/零 HANG」，本轮**不驱动**，`pipeToTicks` 只覆盖未触发扩容的前 5 条写入。
5. `dump_bars_to_file`（`:2020-2215`）的 min1/min5 迁移主体与 `dump_bars_via_dumper`（`:1698`）：先从 `getKlineBlock(..., false)` 拿 RT `.dmb` 块再落 `his/min1|5`，且尾部（`:2180-2215`，本轮未读）有 tick 分支与 dumper 回调 → 只覆盖了 `ct == NULL` 入口一行；其余**待第五波**（需先读完 `:2180-2215` 再决定，避免盲断言）。
6. `transHisData`（`:1569`）、`DataManager::preloadRtCaches`（`:218`，源码已整段注释，不可覆盖）、`onMinuteEnd`/`_proc_date` 的日期推进（依赖 `TimeUtils::getCurDate()` 墙钟，本波纪律禁止断言墙钟）→ **排除**。
7. 简报点名但本目录不存在：`WtHisDatas`、`WtdayDatas`、`WtTSTickData`（同 `manifests/p05_datastorage2.md` 跳过项 4，沿用结论）。

## 门禁结果（唯一构建动作：`-fsyntax-only`，未跑 cmake/ninja，未自行编译运行用例）

命令（仓库根，逐文件）：
```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/p10_storage_writer/<file>.cpp
```
- `p10_day_dsb_dump_test.cpp`：**clean（0 error 0 warning）**
- `p10_proc_block_data_test.cpp`：**clean**
- `p10_rt_block_files_test.cpp`：**clean**

过程中修掉的本车道自身问题 3 处（供后续波次参考）：
1. `#define private public` 前必须先把 `Includes/FasterDefs.h`（→ `FasterLibs/ankerl/unordered_dense.h`）等第三方头 include 干净，否则 `error: 'iter_t' redeclared with 'public' access`（类内成员先按默认 private 声明、再被宏改写成 public 定义）。已收口成 `p10_writer_prefix.h`。
2. `wt_strcpy` 的声明头不确定 → 全部换成 `snprintf(dst, sizeof(dst), "%s", src)`。
3. 自查出一条**会越界读**的自身断言（对 12 字节的空载荷 `.dsb` 再从 `data()+12` 读 88 字节），已改成按现状断言（正是 BUG-P10-004）。

## 剩余（可选后续，按性价比排序）

1. `dump_bars_to_file`/`dump_bars_via_dumper` + `pipeToKlines`（约 900 行）：前置条件是「能不经 `init()` 注入 `_tick_cache_block`/`_bd_mgr`」并读完 `:2180-2215`。
2. `loadCache`/`updateCache`/`getCurTick`（约 400 行）：`cache.dmb` 是文件映射（非命名 shm），可测但需最小 `IBaseDataMgr` mock（`getContractSize`/`getContract`）。
3. `resizeRTBlock` 扩容分支：需人为压小 capacity，建议先用 `DISABLED_` 观察，不进启用集。
