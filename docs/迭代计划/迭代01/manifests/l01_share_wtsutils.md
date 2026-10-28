# 清单 — lane L01（src/Share + src/WTSUtils）

- 工作流: 测试覆盖率迭代01 / L01
- 范围: `src/Share/`、`src/WTSUtils/`
- 新增测试目录: `src/TestUnits/coverage/l01_share_wtsutils/`
- 缺陷记录: `docs/迭代计划/迭代01/bugs/l01_share_wtsutils.md`（19 条，状态全部「待审核」）
- 生产代码改动: 无（`src/**` 全程只读，未触碰任何 `CMakeLists.txt`）

## 一、新增文件与覆盖符号

| 文件 | 行数 | 覆盖的类/函数（`src/Share/…`） |
| --- | --- | --- |
| `l01_codehelper_test.cpp` | 372 | `CodeHelper::CodeInfo`（`isExright`/`isForwardAdj`/`isBackwardAdj`/`stdCommID`/`clear`/`hasRule`）、`isStdChnFutOptCode`（状态机全部分支）、`isStdMonthlyFutCode`、`stdCodeToStdCommID2`、`rawMonthCodeToRawCommID`、`rawMonthCodeToStdCode`（4 位/3 位/`isComm` 三条路径）、`rawFlatCodeToStdCode`（code==pid、pid 空、常规拼接）、`isMonthlyCode`、`rawFutOptCodeToStdCode`（正则命中 + 郑商所手工分支）、`stdCodeToStdHotCode`、`stdCodeToStd2ndCode`、`stdFutOptCodeToRawCode`、`indexCodeMonth`、`extractStdChnFutOptCode`（CFFEX / 其它交易所 / SHFE 三分支）、`extractStdCode`（月合约、复权 `-`/`+`、两段代码、普通码、期权转发） |
| `l01_timeutils_test.cpp` | 312 | `getTZOffset`、`getLocalTimeNow`、`getLocalTime`、`getYYYYMMDDhhmmss`、`getDateTime`、`getCurDate`、`getCurMin`、`getWeekDay`、`isWeekends`、`makeTime`（含 `isToUTC`）、`timeToString`（有/无毫秒、0、负毫秒）、`getNextDate`、`getNextMinute`、`getNextMonth`、`timeToMinBar`/`minBarToDate`/`minBarToTime`（含 `static_assert` 编译期覆盖）、`Time32`（三个构造函数 + `from_local_time` + `date`/`time`/`time_ms`/`fmt`）、`Ticker`（`reset`/`seconds`/`milli_seconds`/`micro_seconds`/`nano_seconds`） |
| `l01_converter_charconv_test.cpp` | 295 | `convert::to_str(T)`、`convert::to_str(char*,len,T)`、`convert::from_str(T, const char*)`、`convert::to_int32/to_uint32/to_int64/to_uint64`（含 `static_assert`）；`URLEncode`（ASCII/空格/非 ASCII/空串 + `operator const char*`）、`URLDecode`（`+`、`%XX` 白名单、残缺序列）、`EncodingHelper::preNUm`/`isUtf8`/`isGBK`、`UTF8toChar`、`ChartoUTF8`（纯 ASCII 分支） |
| `l01_objectpool_test.cpp` | 256 | `ObjectPool<T>` 模板：`construct`（POD 值初始化 / 构造体执行 / `std::string`）、`destroy`（析构计数、内存归还）、`release`、地址唯一性与 LIFO 复用、虚拟析构多态删除、多池互不干扰 |

合计 1235 行、4 个 TU。用例总数 84 个 `TEST`（`l01_codehelper_test.cpp` 31、`l01_timeutils_test.cpp` 23、`l01_converter_charconv_test.cpp` 19、`l01_objectpool_test.cpp` 11，行数由 `grep -c "^TEST"` 在磁盘上实测），其中 7 个 `DISABLED_`（对应会导致挂死/崩溃/当前行为错误的缺陷：`DISABLED_to_str_into_caller_buffer_should_not_keep_stale_tail`、`DISABLED_to_str_internal_static_buffer_stale_digits`、`DISABLED_encoding_helper_is_gbk_hangs`、`DISABLED_get_next_minute_should_normalize_full_days`、`DISABLED_get_next_month_negative_months`、`DISABLED_time_to_min_bar_should_roundtrip_for_2033`、`DISABLED_destroy_null_is_undefined_and_rejected`）。

## 二、复用产物（未重写）

- `src/TestUnits/coverage/ws01_share_core/ws01_strutil_test.cpp`（`StrUtil` 全量）、`ws01_test_util.h`：本轮不再覆盖 `StrUtil`，`CodeHelper`/`Converter` 用例间接调用 `StrUtil::split/replace/findFirst`，形成二次命中。
- `src/TestUnits/coverage/ws05_wtsutils/test_ws05_cfgloader.cpp`：`WTSCfgLoader::load_from_json/yaml/file/content` 与 `WTSVariant` 已覆盖，本轮不重复；`StdUtils.hpp` 的 `StdFile::read_file_content` 亦已由该用例覆盖。
- 本轮临时目录策略：4 个新 TU 均为纯逻辑测试，不需要落盘，因此没有创建临时文件，也就无需 TearDown 清理（`BoostFile`/`WtKVCache` 等落盘目标本轮跳过，见第三节）。

## 三、跳过的目标与原因

| 目标 | 原因 |
| --- | --- |
| `src/Share/BoostFile.hpp`、`BoostMappingFile.hpp`、`BoostShm.hpp` | 需要命名共享内存/文件句柄等系统级外部对象（`shared_memory_object::remove`、`close_file`），用例间互相污染、需要 root 级清理与固定对象名，违背「不依赖固定绝对路径 / 确定性」约束；且 `BoostShm` 在非 Linux 上行为不一致。建议后续单独立一个带 fixture（唯一对象名 + TearDown 移除）的工作流。 |
| `src/Share/DLLHelper.hpp` | 依赖真实 `.dylib`/`.so` 产物与导出符号，构建产物路径不固定，无法在语法门禁环境下安全构造。 |
| `src/Share/cppcli.hpp`（1981 行） | vendored 第三方 CLI 解析库，参数校验失败路径大量调用 `std::exit(-1)`/`std::exit(0)`，用例子进程化成本高；属第三方代码，不计入第一方覆盖率口径。 |
| `src/Share/charconv.hpp` 的 `UTF8toChar`/`ChartoUTF8` 非 ASCII 分支 | 依赖 `iconv` 与运行环境本地代码页（macOS 上 GBK/CP936 是否可用取决于 `LC_CTYPE`），断言不确定；只覆盖纯 ASCII 短路分支。 |
| `src/Share/decimal.h`、`fmtlib.h`、`threadpool/`、`fmt_enum_compat.h` | 第三方/头文件适配层（fmtlib、Boost.ThreadPool、`decimal`），非第一方逻辑。 |
| `src/WTSUtils/WtLMDB.hpp` | 需要 LMDB C 源（`src/WTSUtils/lmdb`）参与链接与磁盘环境，且 `WtLMDBQuery` 依赖真实事务；本轮预算内不做。 |
| `src/WTSUtils/WTSCmpHelper.hpp` | 依赖 `zstdlib`（zstd）链接；纯 `throw` 路径需要一个畸形压缩头，构造成本高于本轮预算。 |
| `src/WTSUtils/SignalHook.hpp`、`StackTracer/`、`yamlcpp/`、`zstdlib/`、`lmdb/` | 信号处理/dladdr/第三方 vendored 库，测试会改变进程全局信号语义，不适合放入共享测试进程。 |
| `src/Share/CpuHelper.hpp`、`ProcHelper.hpp`、`ModuleHelper.hpp`、`IniHelper.hpp`、`WtKVCache.hpp`、`StdUtils.hpp`(`StdLocker`)、`SpinMutex.hpp` | **本轮预算内未做完，不是外部依赖问题**，属于剩余待办（见第五节）。 |

## 四、语法门禁结果

命令（仓库根执行，与主控下发的门禁完全一致）：

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/l01_share_wtsutils/<file>.cpp
```

逐个文件复检结果（clang 退出码 + 诊断行数）：

| 文件 | 退出码 | 诊断 |
| --- | --- | --- |
| `l01_codehelper_test.cpp` | 0 | 0 警告 0 错误 |
| `l01_timeutils_test.cpp` | 0 | 0 警告 0 错误 |
| `l01_converter_charconv_test.cpp` | 0 | 0 警告 0 错误 |
| `l01_objectpool_test.cpp` | 0 | 0 警告 0 错误 |

**最终状态：4/4 文件 0 告警 0 错误。**
本机环境：macOS arm64、Apple clang、Homebrew Boost 1.88（`boost/xpressive`、`boost/pool` 可用）、`/opt/homebrew/include` 提供 fmt。

未运行 cmake/ninja、未运行 gtest 二进制（按任务约定由主控做全量构建与执行）。新增 4 个 TU 尚未接入 `src/TestUnits/CMakeLists.txt`（该文件只读，未修改），需主控把 `coverage/l01_share_wtsutils/*.cpp` 加入测试源列表。

## 五、剩余待办（下一轮 L01 可直接接手）

1. `CodeHelper::extractStdCode` 的 `IHotMgr*` 非空分支（`_ruletag` 路径、`hasRule()==true`）——只需实现一个 fake `IHotMgr`（纯虚接口）即可覆盖，当前全部传 `nullptr`。
2. `CodeHelper::find`（private）、`rawMonthCodeToStdCode` 超长输入、`extractStdCode` 无 `.` 输入：等 BUG-L01-001/013/016 修复后解禁对应用例。
3. `src/Share/IniHelper.hpp`（420 行，`read_ini`/`write_ini`/`isLoaded`/各类型 getter）——可用 `std::filesystem::temp_directory_path()` 下 `l01_` 前缀唯一目录做确定性文件测试，投产比高，建议下一轮优先。
4. `src/Share/StdUtils.hpp` 的 `StdLocker`、`StdFile::write_binary_content/read_binary_content`（异常路径 L62/L71/L90/L107/L120/L131 均可用临时文件精确触发）。
5. `src/Share/SpinMutex.hpp`（`SpinMutex`/`SpinLock`，67 行）与 `ProcHelper.hpp`/`CpuHelper.hpp`（`getPid`/`isRunAsService` 等只读系统调用）。
6. `src/Share/WtKVCache.hpp`（277 行）——依赖 `BoostMappingFile`，若要覆盖需先解决第三节说明的共享内存 fixture 问题。
7. `EncodingHelper::isGBK` 的 `false` 分支覆盖（阻塞在 BUG-L01-005 死循环）。

## 六、缺陷摘要（详见 bugs 文件）

19 条「待审核」：Blocker 6（`find` 下标回绕 / `isGBK` 死循环 / `timeToMinBar` 2032 后溢出 / `from_str(std::string&)` 无法编译 / `extractStdChnFutOptCode` 长度 -1 / `extractStdCode` 越界）、Major 7、Minor 6。
为了让 4 个 TU 通过语法门禁，本轮没有为了变绿而放宽任何断言：可疑行为一律按「实际行为 + 注释」断言，并把期望行为放进 `DISABLED_` 用例。
