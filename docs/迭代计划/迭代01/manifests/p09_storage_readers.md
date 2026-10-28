# p09_storage_readers 交付清单 — WtDataStorage 读侧本体（第四波 lane p09）

- 车道: p09（主目标二进制 `CoverageTests`，`src/TestUnits/coverage/CMakeLists.txt` 的 `MAIN_PLUGIN_TUNS` 已编入 `src/WtDataStorage/*.cpp`）
- 靶子: `src/WtDataStorage/WtRdmDtReader.cpp`（2331 行未覆盖 / 0.26%）、`src/WtDataStorage/WtDataReader.cpp`（1846 行未覆盖 / 3.65%）
- 生产代码改动: **无**。只新增 `src/TestUnits/coverage/p09_storage_readers/`（3 个 .cpp）与 `docs/迭代计划/迭代01/{bugs,manifests}/p09_storage_readers.md`；未动任何 `CMakeLists.txt`
- 与 l04/p05 的边界: 不碰 `proc_block_data`/`.dsb` 驱动 `WtBtDtReader`/块头版本矩阵/LMDB key（l04 已覆盖），不碰 `WtBtDtReaderAD`/`WtRdmDtReaderAD`（p05 已覆盖），**非 AD 本体 only**（两个目录的同名 `extern "C"` 工厂不能进同一二进制）

## 新增测试文件（3 个，1274 行，25 个 TEST_F，其中 3 个 `DISABLED_`，约 165 条断言宏）

| 文件 | 行数 | 主题 |
|---|---|---|
| `p09_rdm_bars_cache_test.cpp` | 497 | `WtRdmDtReader` 缓存索引层（init/区间/计数/拷贝三族）+ 复权因子层（`loadStkAdjFactorsFromFile`/`getAdjFactors`/`getAdjFactorByDate`/`clearCache`） |
| `p09_rdm_window_range_test.cpp` | 386 | `WtRdmDtReader` 区间裁剪语义：左右闭区间、右端越界、左端越界、`stime=0`、跨段拼接重复 bar、`isDay` 与非 `isDay` 两条比较器、`_rt_bars` 不参与索引、`count==0/1/超量` |
| `p09_reader_init_adjfactor_test.cpp` | 391 | `WtDataReader::init` 配置解析（NULL cfg / 单 path / path 数组 / `his_path` / `adjust_flag` / `adjfactor` 文件）+ 复权因子全链 |

落盘全部在 `std::filesystem::temp_directory_path()/p09_{rdm,rd}_<pid>_<seq>`（json 配置与复权因子文件），
`TearDown` 用 `remove_all(ec)` 清理；**未读未写仓库 `dist/`**；不依赖上次运行残留（序号 + pid 保证唯一）。
`p09_rdm_window_range_test.cpp` 纯内存，不落盘。

## 覆盖的类 / 函数 / 符号（符号名全部来自 api-brief-L04 或本轮工具结果）

WtRdmDtReader（`src/WtDataStorage/WtRdmDtReader.cpp`）
- `init(cfg==NULL, sink)`（`:69`，注入 `_sink/_base_data_mgr/_hot_mgr` 后提前返回；**不创建 `_thrd_check`**）
- `indexBarFromCacheByRange`（`:1629` 空缓存守卫 / 中间窗口 / 右端 `eit==end` / 单点窗口 / 左端整体越界 / `isDay` 比较器）
- `indexBarFromCacheByCount`（`:1688` `min()` 收敛 / 起点回到 0 / 右端越界 / `count==0` 分支）
- `readBarsFromCacheByRange`（`:1729` 拷贝语义逐字段比对 / `eit==begin()` 返回 0 / 空区间不 `resize` / `isDay` 分支）
- `clearCache`（`:2643`，`_bars_cache` 清空、`_adj_factors` 保留）
- `loadStkAdjFactorsFromFile`（`:169` 文件不存在 / json 解析失败 / 带 PID 与不带 PID 的 key 拼装 / 非数组成员 `continue` / `19900101` 兜底条目 / `std::sort`）
- `getAdjFactors`（`WtRdmDtReader.h:536` inline，命中与未命中）
- `getAdjFactorByDate`（`:2607` 非股票 1.0 / 空因子表 1.0 / 命中 / 夹在中间 / 超出末条取 `back()` / 首条）
- 私有类型 `WtRdmDtReader::BarsList`（`_bars`/`_rt_bars`/`_exchg`/`_code`/`_period`/`_factor`）、`AdjFactor`/`AdjFactorList`/`AdjFactorMap`、成员 `_bars_cache`/`_adj_factors`/`_base_dir`/`_thrd_check`/`_rt_min1_map`/`_rt_tick_map`

WtDataReader（`src/WtDataStorage/WtDataReader.cpp`）
- `init`（`:151` cfg==NULL 提前返回、`path` 单值派生 `rt/`+`his/`、`path` 数组多目录、`his_path` 追加、`adjust_flag`、`adjfactor` 文件加载与「No adjusting factor file configured」分支、sink 日志落点）
- `loadStkAdjFactorsFromLoader`（`:219` `_loader==NULL` 短路）
- `loadStkAdjFactorsFromFile`（`:252` 三种失败 + key 规则 + 排序 + 兜底条目）
- `getAdjFactors`（`:2349` public，未命中且无 loader 返回空表）
- `getAdjFactorByDate`（`:2313` 四个分支）、`getAdjustingFlag`（`WtDataReader.h:215` inline）
- 私有成员 `_rt_dir`/`_his_dir`/`_adjust_flag`/`_adj_factors`/`_loader`、嵌套类型 `AdjFactor`/`AdjFactorList`

宿主注入面（本车道自带最小实现，不引 `WtCore/WtUftCore/WtBtCore/WtDtCore`）
- `IBaseDataMgr`（含 `getCommodity` 双重载、`calcTradingDate`、`getBoundaryTime`）、`IHotMgr`（14 个纯虚）、
  `IRdmDtReaderSink`、`IDataReaderSink`（`on_bar`/`on_all_bar_updated`/`get_date`/`get_min_time`/`get_secs`/`reader_log`）
- `WTSCommodityInfo::create`/`setCategory(CC_Stock|CC_Future)`/`setSessionInfo`、`WTSSessionInfo::create`/`addTradingSection`
- `WTSCfgLoader::load_from_file`（`src/WTSUtils/WTSCfgLoader.h`，json cfg 与复权因子文件解析）

## 白盒手法说明（评审要点）

`indexBar*`/`readBarsFromCacheByRange`/`loadStkAdjFactorsFromFile`/`cacheHisBarsFromFile` 及成员表
`_bars_cache`/`_adj_factors` 在生产头里都是 **private**（`WtRdmDtReader.h:44/271/492`、`WtDataReader.h`），
公开入口 `readKlineSliceByRange/readTickSlice*` 又必须先取 `rt/` 下的 `BoostMappingFile` **命名共享内存块**。
为使「日期区间/窗口裁剪/增量」这类纯计算分支可测，本车道在 include 目标类头文件处使用：

```cpp
#define private public
#define protected public     // p09_reader_* 另在 Includes/IDataReader.h 处单独放开
#include "WtDataStorage/WtRdmDtReader.h"
#undef private
#undef protected
```

- 三个被 include 的类头自身依赖（`DataDefine.h`/`FasterDefs.h`/`IRdmDtReader.h`/`BoostMappingFile.hpp`/
  `StdUtils.hpp`/`fmtlib.h`）都在宏之前按正常权限请进来，宏只作用于目标类定义本身，避免污染 Boost；
- C++ 访问权限不进入 Itanium name mangling，链接到的仍是 `libWtDataStorage` 里的真实实现（非副本、非改写）；
- 未修改任何生产源码，未新增 TU 需求，未动 `CMakeLists.txt`。
  若评审不接受该手法，等价替代是给这三个函数加 `friend class`（属生产代码改动，需单独评审）。

## 跳过目标及原因（供排除清单评审）

1. **RT 命名共享内存块全链**：`getRTKilneBlock/getRTTickBlock/getRTOrdQueBlock/getRTOrdDtlBlock/getRTTransBlock`
   （`WtRdmDtReader.cpp:2002-2219`、`WtDataReader.cpp:1920-2210`）与依赖它们的
   `readKlineSliceByRange/ByCount`（`:1789`、`:2219`）、`readTickSliceByDate/ByRange/ByCount`（`:239`、`:363`、`:2395`）、
   `readOrd*/readTrans*SliceByRange`（`:611`、`:786`、`:960`）、`WtDataReader::read*Slice`（`:322-930`、`:1720`）
   —— 都要求 `rt/` 目录下可 mmap 的 `BoostMappingFile` 段（writer 侧才能建），按派工「共享内存 RT 块跳过并点名」处理。
2. **`_thrd_check` 5s 轮询线程与 `WtDataReader::onMinuteEnd`**（`WtRdmDtReader.cpp:87-165`、`WtDataReader.cpp:2210`）：
   派工点名「禁止阻塞路径 / 后台线程」；`init(cfg!=NULL)` 会 `new StdThread` 且析构 `join`，
   一条用例至少多付 5s 且引入跨线程访问 `_rt_*_map` 的竞争。
   （顺带纠偏：p05 清单说 `WtRdmDtReader::init` **无条件**启动轮询线程，实测 `:76` 有 `if (cfg == NULL) return;`，
   线程只在 cfg 非空时创建，所以本车道的 `init(NULL, sink)` 是零线程、零 join 的安全入口。）
3. **`cacheHisBarsFromFile`（`WtRdmDtReader.cpp:1134-1628` 约 495 行）/ `cacheIntegratedBars`/`cacheAdjustedStkBars`/
   `cacheFinalBarsFromLoader`/`cacheHisBarsFromFile`（`WtDataReader.cpp:969/1287/930/1564`）**：
   形参 `void* codeInfo` 实为 `CodeHelper::CodeInfo*`（`_exchg/_product/_code/_ruletag/_exright/stdCommID()/isExright()`），
   且非 HOT 分支之后要接 RT 块与 `HotSections`；本轮未定位 `CodeHelper` 头文件路径
   （`grep -rn "struct CodeInfo" src/Includes/` 无命中），预算内不再扩展，留给下一波（可拿到 ~500 行，是这两个文件的剩余主体）。
4. `WtDataWriter.cpp`（2700 行）：`init` 起 `StdThread(check_loop)`，内含 `sleep_for(10s)`/`sleep_for(300s)`（`:1621`、`:2385`）
   → 直接撞 HANG 门禁，按派工排除。

## 疑似缺陷

`docs/迭代计划/迭代01/bugs/p09_storage_readers.md`，5 条，全部状态「待审核」：
- BUG-P09-001（Blocker）`indexBarFromCacheByRange/ByCount` 在**公开默认参数 `etime=0`** 下 uint32 回绕使 `lower_bound` 命中 `begin()`，`eit--` 缺守卫 → 迭代器/索引回绕越界（同文件 `readBarsFromCacheByRange:1762` 有守卫）
- BUG-P09-002（Major）`readBarsFromCacheByRange` 缺 `_bars.empty()` 守卫 → 未预热 key 上 `size()-1` 下溢 + `resize(天文数字)`
- BUG-P09-003（Major）`WtDataReader::init` 未判 `cfg->get("path")` 返回值即 `cfgPath->isArray()` → 空指针解引用
- BUG-P09-004（Minor）`getAdjFactors` 的 `thread_local static char key[20]` 无界 `format_to`（rdm/reader 两处同型）
- BUG-P09-005（Minor）空区间条数依赖 `size_t` 环绕才得 0；`indexBarFromCacheByCount` 在 `count==0` 返回一位越界指针

崩溃/UB/空指针类输入（BUG-P09-001/002/003 的触发路径）**全部收进 3 个 `DISABLED_` 用例**，
常规 22 条用例零残留、零阻塞；本仓库自带 gtest 无 `GTEST_SKIP`，`DISABLED_` 用例携带真实复现代码，
只在 `--gtest_also_run_disabled_tests` 下执行。

## 语法门禁（唯一构建动作，未跑 cmake/ninja/测试二进制）

命令（仓库根，逐个文件）：
```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/p09_storage_readers/<file>.cpp
```
结果：**3/3 clean（0 error 0 warning）**，最后一轮三行 `GATE[...]` 后均无任何编译器输出。
过程中修掉本车道自身问题 6 处：`WTSBarStruct` 无 `cht/total_amount/amount` 字段（只保留 date/time/OHLC/vol）、
`std::filesystem::error_code` → `std::error_code`、`_sink`/`_loader` 是基类 protected 成员（改为经 sink 日志计数断言 /
在 `Includes/IDataReader.h` 处单独 `#define protected public`）、`IDataReaderSink` 还有 `get_date/get_min_time/get_secs`
三个纯虚、`WTSCfgLoader` 需 `#include "WTSUtils/WTSCfgLoader.h"`、`WtRdmDtReader::init` 误传 sink 当 cfg（类型不兼容）。

## 剩余（可选后续）

1. `cacheHisBarsFromFile` 族（rdm ~495 行 + reader ~660 行）：需先确认 `CodeHelper::CodeInfo` 的头路径与
   `.dsb` 块头构造（l04 已有 `BlockHeader/HisKlineBlockV2` 布局可复用），可再拿 ~1100 行，是本模块最大剩余缺口。
2. RT 共享内存块：需要一个可注入的非命名 `BoostMappingFile` 替身（属生产代码改造，需单独评审）。
3. `_thrd_check` 5s 轮询：等 `_stopped` 能被配置化提前退出后再测，否则每条用例多付 5s。
