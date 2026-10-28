# p05_datastorage2 交付清单 — WtDataStorage / WtDataStorageAD（第三波 lane p05）

- 车道: p05（目标二进制 `CoverageTests_storage`，编译 `src/WtDataStorage/*.cpp` + `src/WtDataStorageAD/*.cpp`）
- 模块: `src/WtDataStorageAD`（读侧为主）；`src/WtDataStorage` 本轮未新增覆盖（原因见「跳过目标」）
- 生产代码改动: **无**。只新增 `src/TestUnits/coverage/p05_datastorage2/`（3 个 .cpp）与 `docs/迭代计划/迭代01/{bugs,manifests}/p05_datastorage2.md`；未动任何 `CMakeLists.txt`
- 额外 TU 需求: **无**（只 include 头文件：`Includes/*`、`Share/TimeUtils.hpp`、`WTSUtils/WtLMDB.hpp`、`WtDataStorageAD/*`）

## 新增测试文件（3 个，1260 行，26 个 TEST_F/TEST，其中 4 个 DISABLED_，133 条断言宏）

| 文件 | 行数 | 主题 |
|---|---|---|
| `p05_btdtreader_ad_lmdb_test.cpp` | 537 | `WtBtDtReaderAD` 的 LMDB 建库→读回→逐字段比对（bars/ticks、哨兵区间、交易所隔离、db 缓存） |
| `p05_rdmdtreader_ad_slices_test.cpp` | 530 | `WtRdmDtReaderAD::readKlineSliceByRange` 全量/增量/窗口裁剪 + 自带 `IBaseDataMgr`/`IHotMgr`/`IRdmDtReaderSink` mock |
| `p05_minbar_key_overflow_test.cpp` | 193 | AD 落库键尾 `uint32` 截断与 `0xffffffff` 上界哨兵的日期回绕（纯 LMDB + 算术） |

> 行数超出简报的 150–400 建议区间：单条用例做了整段二进制逐字段比对（15 根 bar × 8 字段），为把「一条用例一个进程」的启动成本摊薄而合并了断言，未拆细。

落盘全部在 `std::filesystem::temp_directory_path()/p05_{btdt,rdmad,keytail}_<pid>_<seq>`，`TearDown` 先销毁 reader（释放只读 LMDB mmap）再 `remove_all(ec)`；未读未写仓库 `dist/`。

## 覆盖的类 / 函数 / 符号（符号名全部来自 api-brief-L04 或本轮工具结果）

WtDataStorageAD
- `WtBtDtReaderAD`：`init(cfg==NULL / cfg!=NULL)`、`read_raw_bars`（`get_k_db` 的 `min1|min5|day` 三个 subdir 分支、非法周期返回空 ptr、空库、缺目录、按 `sizeof(WTSBarStruct)` 拼 buffer）、`read_raw_ticks`（`(uDate, 0) ~ (uDate, 240000000)` 哨兵、交易日分桶、夜盘 210000000 时段）、`get_k_db`（`_exchg_m1_dbs/_exchg_m5_dbs/_exchg_d1_dbs` 命中与 `std::move` 掏空）、`get_t_db`（查表键 `exchg.code` vs 写表键 `exchg`）
- `WtRdmDtReaderAD`：`init`、`readKlineSliceByRange`（`barsList._bars.empty()` 全量重载分支 / `bNeedNewer` 增量分支 / `lower_bound` 右端裁剪与 `pBar--` 空窗 / `isDay` 与非 `isDay` 两条比较器 / `WTSKlineSlice::create` 输出块）、`readTickSliceByCount`、`readTickSliceByDate`、`readKlineSliceByCount`（三个 TODO 桩）、`readOrdDtlSliceByRange`/`readOrdQueSliceByRange`/`readTransSliceByRange`（头文件桩）、`getAdjFactorByDate`、`clearCache`、`get_k_db`/`get_t_db`（含 `fs::create_directories` 自愈）
- `CodeHelper::extractStdCode`（两段/三段标准代码 → `_exchg/_product/_code`）、`WTSCommodityInfo::create`、`WTSSessionInfo::create`/`addTradingSection`、`WTSKlineSlice::get_block_counts/get_block_addr/get_block_size`
- `LMDBBarKey`/`LMDBHftKey` 键尾截断语义（`(uint32_t)bar.time`）、`WtLMDB`（读写 + 只读两次打开同一目录）、`WtLMDBQuery::put/commit/get_range`
- `TimeUtils::timeToMinBar`；`IError`/`IBaseDataMgr`/`IHotMgr`/`IRdmDtReaderSink`/`IBtDtReaderSink` 的本车道最小实现

## 跳过目标及原因（供排除清单评审）

1. **`WtDataWriter`（2700 行）全部**：`init` 会 `new StdThread(check_loop)`，`check_loop` 内含 `sleep_for(10s)` 与 `sleep_for(300s)`（`WtDataWriter.cpp:1621`、`:2385`），`writeTick` 走 `proc_loop` 后台线程 + `BoostMappingFile` **命名共享内存** RT 块（`getKlineBlock/getTickBlock/resizeRTBlock`）。属简报点名的「多进程/共享内存」分支，且 300s 清理循环会直接撞本波的 HANG 门禁 → 排除。
2. **`WtDataWriterAD`（1073 行）全部**：`_async_task` **从未被初始化也从未被赋值**（全目录只出现 2 处：`WtDataWriterAD.h:245` 声明、`.cpp:375` 读取 → BUG-P05-005），`writeTick` 因此随机进入线程分支；`release()` 无条件 `join` 一个可能 park 在无谓词 `_task_cond.wait()` 上的线程（丢唤醒即永久阻塞）。为保证「零 HANG」，本车道不驱动任何会进入 `pushTask` 线程分支的接口。AD writer 的 `loadCache`/`updateBarCache`/`pipeTo*`/`resizeRTBlock` 需要先把 `_async_task` 与 condvar 修好才能安全覆盖。
3. **`WtDataReader`（2391 行）/`WtRdmDtReader`（2653 行）主体**：`readTickSlice*`/`readKlineSlice*` 先取 RT 共享内存块（`getRTKilneBlock/getRTTickBlock`）再拼历史文件；`WtRdmDtReader::init` 无条件启动 5s 轮询线程并在析构 `join`（1 进程 1 用例 = 每条用例至少 5s 阻塞）。`indexBarFromCacheByRange/ByCount` 是 private，无法在无共享内存的情况下从公开入口稳定喂到「空区间」输入 → 本轮只做缺陷记录（BUG-P05-007）。
4. **简报里点名但目录里不存在的文件**：`WtHisDatas`、`WtdayDatas`、`WtTSTickData` 不在 `src/WtDataStorage/`（该目录实测只有 `WtBtDtReader.cpp`、`WtDataReader.cpp`、`WtDataWriter.cpp`、`WtRdmDtReader.cpp` 4 个 TU，共 10014 行含头文件）。按这三个名字去写用例会整块落空，建议主控核对简报清单。

## ⚠ 链接期风险（必须主控处理，本车道不改 CMakeLists）

`src/WtDataStorage/` 与 `src/WtDataStorageAD/` 各自用 `extern "C" { EXPORT_FLAG ... }` 导出**同名 8 个 C 符号**（实测 `grep -n EXPORT_FLAG`）：
`createBtDtReader/deleteBtDtReader`（`WtBtDtReader.cpp:62,72` vs `WtBtDtReaderAD.cpp:28,34`）、`createDataReader/deleteDataReader`（`WtDataReader.cpp:34,40` vs `WtDataReaderAD.cpp:34,40`）、`createWriter/deleteWriter`（`WtDataWriter.cpp:43,49` vs `WtDataWriterAD.cpp:36,42`）、`createRdmDtReader/deleteRdmDtReader`（`WtRdmDtReader.cpp:36,42` vs `WtRdmDtReaderAD.cpp:35,41`）。
C++ 侧 `FACT_NAME` 在这两个目录确实没有（0 处），但 **extern "C" 工厂函数不看 C++ 名字空间**。一旦两条 `*.cpp` 被放进同一个可执行目标（l04 的用例会拉入 `WtBtDtReader.o`，本车道拉入 `WtBtDtReaderAD.o`，两个对象都定义 `createBtDtReader`），链接期就是 `duplicate symbol`。建议：两个目录仍分别成库并把重复对拷进各自库、用 `-Wl,-allow_multiple_defs` 之类是危险的（会静默取一份，等于 BUG-L00-005 的复现）；**更安全的是只把 `src/WtDataStorageAD/*.cpp` 编进 `CoverageTests_storage`**，或给 `src/WtDataStorage` 的 4 个工厂加 `static`/重命名的改造（属生产代码改动，本迭代禁止，需单独评审）。

## 疑似缺陷（不修源码，全部「待审核」）

`docs/迭代计划/迭代01/bugs/p05_datastorage2.md`，7 条：
- BUG-P05-001（Blocker）`get_k_db` 用 `return std::move(it->second)` 掏空 db 缓存 → 同一 `(exchg,period)` 第 3 次取数静默返回 false/NULL
- BUG-P05-002（Major）`readKlineSliceByRange`/`readTickSliceByRange` 尾部对空 `vector` 取 `&v[0]` 且 `size()-1` 下溢 → 段错误（5 类输入收进同一个 DISABLED_ 用例）
- BUG-P05-003（Major）`get_t_db` 查表键 `exchg.code` 与写表键 `exchg` 不一致 → tick db 缓存永不命中、每次重开 LMDB 环境
- BUG-P05-004（Major）分钟线键尾 `uint32` 截断：`bar.time` 自 20330101 起 > 2^32-1，回绕并与早年键空间覆盖写
- BUG-P05-005（Major）`WtDataWriterAD::_async_task` 未初始化 + 无谓词 condvar `wait` → 随机线程分支与 `release()` 丢唤醒死锁（因此 AD writer 主体未覆盖）
- BUG-P05-006（Major）`WtBtDtReaderAD::read_raw_bars/read_raw_ticks` 未校验 value 长度即按 `sizeof(结构体)` memcpy → 越界读/写（4 类输入收进同一个 DISABLED_ 用例）
- BUG-P05-007（Major）`WtRdmDtReader::indexBarFromCacheByRange/ByCount` 缺 `eit == begin()` 保护（同文件 `readBarsFromCacheByRange` 有）→ `eit--` 下越界 + 计数回绕

越界/崩溃路径（BUG-P05-002/006 的触发输入）与死锁路径（BUG-P05-005）**故意不进入常规执行**，全部收进 4 个 `DISABLED_` 用例；BUG-P05-001 的理想行为也已写成 2 个 `DISABLED_` 用例（修复后可直接启用，不靠放宽断言变绿）。

## 语法门禁（唯一构建动作）

命令（仓库根，逐个文件）：
```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/p05_datastorage2/<file>.cpp
```
结果：**3/3 clean（0 error 0 warning）**，最后一轮复跑见对话记录（`FINAL-GATE[...]=clean`，三行均无编译器输出）。未跑 cmake/ninja/测试二进制。

过程中修掉的本车道自身问题 5 处：本仓库自带 gtest **没有 `GTEST_SKIP`**（DISABLED_ 用例改为携带真实复现代码，只在 `--gtest_also_run_disabled_tests` 下执行）；`&make_tick(...)` 取临时对象地址（`-Waddress-of-temporary`）；`WTSSessionInfo` 必须**先于** `WTSContractInfo.hpp` include（后者只前向声明，BUG-L00-004 同型）；`WTSKlineSlice` 无 `count()`，改用 `get_block_size(0)`；`unique_ptr<mock>` 遇上 `IBaseDataMgr`/`IHotMgr` 非虚析构 → `-Wdelete-non-abstract-non-virtual-dtor`，给三个 mock 补显式虚析构。

## 剩余（可选后续）

1. `WtDataWriter` 的 `dump_bars_to_file` / `dump_day_data` / 块合并：等 `_proc_chk`/`check_loop` 的 300s 分支能被配置关闭（或 RT 块可由测试注入非命名共享内存）后再做，预计 1–2 个文件、覆盖 writer 侧 ~800 行。
2. `WtDataWriterAD` 的 `updateBarCache`（D1/M1/M5 聚合，~350 行纯计算）：前置条件是 BUG-P05-005 修复；修复后本车道可直接用「writeTick → 读 LMDB → 逐字段比对」的 round-trip 覆盖，并与 `WtRdmDtReaderAD` 串成端到端。
3. `WtRdmDtReader::indexBarFromCacheByRange/ByCount` + `cacheHisBarsFromFile` 的非 HOT 分支（~400 行）：需要能绕开 `init` 的 5s 轮询线程（例如把 cfg 的 `adjfactor` 单独喂入），否则每条用例多付 5s。
