# 缺陷记录 — lane L05（WtDtCore / WtDtHelper / WtDtServo / WtDtPorter）

> 本迭代只记录缺陷，不修改生产代码；状态一律「待审核」。
> 相关测试位于 `src/TestUnits/coverage/l05_dt_servo_porter/`。

## BUG-L05-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtDtHelper / `src/WtDtHelper/WtDtHelper.cpp` `strToDate()`（L146-163）
- 相关测试: `l05_dt_helper_bar_test.cpp: TransCsvBarsDailyRoundtrip`（当前只喂入 3 段/1 段日期，未覆盖 2 段分支）
- 输入/前置条件: CSV 的 date 列为 `2020/01` 或 `2020-01`（切分后只有 2 个元素）
- 期望行为: 参数不足时应返回 0 或通过日志报错，不越界访问
- 实际行为: `if (ay.size() > 1)` 后无条件访问 `ay[2]`，对 `StringVector` 越界读（UB），可能崩溃或读到脏字符串
- 复现步骤: 构造 `date=2020/01,time=0,...` 的 CSV 并调用 `trans_csv_bars`
- 影响范围: 所有 CSV→二进制转换入口（trans_csv_bars 等），外部 Python/命令行用户
- 备注: 不阻塞覆盖率目标；建议把判断改为 `ay.size() >= 3`，否则走单元素分支。

## BUG-L05-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtDtHelper / `src/WtDtHelper/WtDtHelper.cpp` `store_bars`(L1077)、`store_ticks`(L1117)、`store_order_details`(L1157)、`store_order_queues`(L1197)、`store_transactions`(L1237)、`trans_csv_bars`(L464-470)
- 相关测试: `l05_dt_helper_bar_test.cpp: StoreBarsSwallowsFileCreationFailure`（按现状断言返回 true，并记录该缺陷）
- 输入/前置条件: 目标目录不存在或磁盘只读，`BoostFile::create_new_file()` 失败
- 期望行为: 返回 false（store_*）并输出失败日志，调用方能感知落盘失败
- 实际行为: 返回值未校验，`bf.close_file()` 后仍 `return true`；`trans_csv_bars` 更严重——`bf.write_file(cmprsData)` 写在 `if (create_new_file)` 判断之外，建文件失败时向无效句柄写入，静默丢数据
- 复现步骤: `store_bars("<temp>/no_such_dir/x.dsb", bars, 1, "d", NULL)` → true，但文件不存在
- 影响范围: 数据收割/转储链路的静默数据丢失
- 备注: 建议统一检查 `create_new_file` 返回值并把 `write_file` 放进同一分支。

## BUG-L05-003

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: WtDtHelper / `src/WtDtHelper/WtDtHelper.cpp` `read_dsb_bars`(L735)、`read_dsb_ticks`(L595)、`read_dsb_order_details`(L630)、`read_dsb_order_queues`(L665)、`read_dsb_transactions`(L700)、`dump_bars`(L215)、`dump_ticks`(L304)
- 相关测试: `l05_dt_helper_bar_test.cpp: ReadBarsHeaderValidation`、`l05_dt_helper_tick_test.cpp: ReadTicksHeaderValidation`
- 输入/前置条件: ①外部传入 `firstBar/firstTick = NULL` 且 `count > 0`；②`.dsb` 文件的压缩块长度与 `BlockHeaderV2::_size` 不一致
- 期望行为: C 导出边界应做入参校验并返回错误；`proc_block_data()` 返回 false 时立即终止并报错
- 实际行为: `store_*` 直接 `memcpy(items, NULL, n)` 崩溃；`proc_block_data()` 的返回值在全部 7 处调用点被忽略，解压失败后仍按 `content.size()/sizeof(结构体)` 解析并把垃圾内存交给回调，`dump_ticks` 还不校验块类型（把 K 线 `.dsb` 当 tick 解析）
- 复现步骤: `store_ticks(p, NULL, 3, NULL)`；或写入 `_size` 与实际长度不符的 `.dsb` 后调用 `read_dsb_ticks`
- 影响范围: Python/Porter 侧调用者可稳定触发进程崩溃；坏文件导致脏行情数据入库
- 备注: 建议 `store_*` 增加 `firstItem == NULL || count <= 0` 前置校验；`read_*` 检查 `proc_block_data` 返回值；`dump_ticks` 按 `_type` 过滤。

## BUG-L05-004

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtDtCore / `src/WtDtCore/StatHelper.hpp`（L89-124）
- 相关测试: `l05_dt_core_util_test.cpp: L05DtCoreStatHelper.*`
- 输入/前置条件: 任意 TU 直接 `#include "WtDtCore/StatHelper.hpp"`
- 期望行为: 头文件可独立编译，统计更新在写锁保护下进行且标志位有实际用途
- 实际行为: `BoostRWMutex/BoostWriteLock/BoostReadLock` 三个别名全仓库没有定义（`grep -rl BoostRWMutex src` 只命中该头文件与我的测试），单独包含必编译失败；`updateStatInfo` 中 `flag`/`UF_Recv`/`UF_Send` 计算后被丢弃；`-Wall -Wextra -Werror` 下触发 `-Wunused-but-set-variable`（测试侧只能局部 `#pragma clang diagnostic ignored` 才能编过）
- 复现步骤: `clang++ -std=c++17 -fsyntax-only -Isrc -include 别名定义 <只包含 StatHelper.hpp 的 TU>`
- 影响范围: 该头文件目前是事实上的死代码（ShmCaster/UDPCaster 未包含它），统计数据无处落地
- 备注: 建议在 `Share/` 中补齐三个别名定义并删除无用的 flag 计算；另：溢出分支把 `_send_bytes` 直接赋值为增量（而非回绕/拒绝），统计口径需产品确认。

## BUG-L05-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtDtPorter / `src/WtDtPorter/WtDtPorter.cpp` `write_log()`（L83-93）
- 相关测试: `l05_dt_porter_capi_test.cpp: WriteLogWithAndWithoutCategory`（只覆盖 `""` 与非空分类，未用 NULL 触发崩溃）
- 输入/前置条件: 外部调用 `write_log(1, "msg", NULL)`
- 期望行为: 导出 C 边界应把 `catName == NULL` 当作「无分类」处理
- 实际行为: 无条件 `strlen(catName)` → 空指针解引用崩溃
- 复现步骤: 任意语言绑定漏传 catName（默认参数为 NULL）即可复现
- 影响范围: 所有使用 Porter 日志回调的解析器/收割器插件
- 备注: 同类问题：`WtDtServo.cpp` 的 `get_*` 系列在 `kData != NULL` 时无条件调用 `cbCnt(...)`/`cb(...)`，未校验回调是否为 NULL。建议补 NULL 防护。

## BUG-L05-006

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtDtHelper / `src/WtDtHelper/WtDtHelper.cpp` `dump_bars`(L200、L209)、`dump_ticks`(L305)
- 相关测试: `l05_dt_helper_bar_test.cpp: DumpBarsWritesCsvAndSkipsOthers`
- 输入/前置条件: 目录中存在多个 `.dsb`，其中一个头部校验失败或类型不匹配
- 期望行为: 日志指明具体出错文件路径，便于定位坏文件
- 实际行为: `cbLogger(format(u8"文件{}头部校验失败", binFolder))` 打印的是源目录名而不是当前 `path`，同目录下的 `不是K线数据，跳过转换` 同样打印目录名
- 复现步骤: 在 binFolder 放一个 2 字节的 `broken.dsb` 后调用 `dump_bars`
- 影响范围: 排障效率（不影响数据正确性）
- 备注: 建议改用 `path`/`iter->path()`。

## BUG-L05-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDtHelper/WtDtHelper.cpp`（`read_dsb_bars` L735-743、`read_dmb_bars` L768-775，同族的 `read_dsb_ticks`/`read_dsb_ordqueues`/`read_dsb_transactions` 同一写法）
- 相关测试: `CoverageTests!L05HelperBarTest.ReadBarsHeaderValidation`、`CoverageTests!L05HelperTickTest.ReadTicksHeaderValidation`、`CoverageTests!L05HelperTickTest.StoreAndReadOrdQueueRoundtrip`、`CoverageTests!L05HelperTickTest.StoreAndReadTransactionRoundtrip`、`CoverageTests_dtcore!Q05ResampleTest.MissingOrShortFileFailsHeadCheck`
- 输入/前置条件: 传入一个**不存在**的文件路径（或调用方拼错路径 / 文件被提前删除）。
- 期望行为: 走到函数自己写的「头部校验失败」分支 —— `return 0` 并通过 `cbLogger` 回报
  `文件{}头部校验失败`（这条分支就在读文件后的第 3 行，`content.size() < sizeof(HisKlineBlock)`）。
- 实际行为: `StdFile::read_file_content`（`src/Share/StdUtils.hpp:57-63`）在入口就
  `if (!std::filesystem::exists(filename)) throw std::runtime_error("File does not exist");`
  —— 异常直接穿出一个返回 `WtUInt32` 的 C 风格 API，永远走不到自己写的那条容错分支。
  用例侧表现为 gtest `unknown file: Failure / C++ exception with description "File does not exist"`。
- 复现步骤: `CoverageTests --gtest_filter=L05HelperBarTest.ReadBarsHeaderValidation`
  （该用例第二段就是「文件不存在」这一支，注释已写明期望 `return 0`）。
- 影响范围: 所有把 `read_dsb_*`/`read_dmb_*` 当「返回 0 即无数据」调用的调用方（报表导出、补数、回放前置检查）
  会遇到未捕获异常终止进程；`-fexceptions` 关掉的编译配置下更会直接终止。
  同一个 `read_file_content` 也被存储层直接用（`src/WtDataStorage/WtRdmDtReader.cpp`、`WtDataReader.cpp` 各 10+ 处），
  所以这条是跨模块的口径分歧，不只是 DtHelper。
- 备注: 本轮不改产品代码；5 条用例保持红。修复方向（待评审）二选一：
  要么在读之前判存在性并让「不存在」归入「头部校验失败 → 0」，要么把这些 API 的契约明确改成「可能抛异常」并让调用方捕获。

## BUG-L05-008

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDtHelper/WtDtHelper.cpp`（`read_dsb_bars` L755 `cbCnt(0)`、L759 `cb(...)`）
- 相关测试: 同族 `read_dsb_*` 用例（`CoverageTests!L05Helper*`）
- 输入/前置条件: 只传 `cbLogger`、不传 `cb`/`cbCnt`（三个回调参数都没有默认值，但都是函数指针，调用方可以传 NULL）。
- 期望行为: 与 `cbLogger` 一致 —— 函数体里对 `cbLogger` 每次都写了 `if (cbLogger)` 守卫（L737、L746、L763），
  对 `cb`/`cbCnt` 也应有同样的判定。
- 实际行为: `cbCnt(0)`（内容为空的分支）与 `cb((WTSBarStruct*)content.data(), kcnt, true)` 无条件调用 →
  传 NULL 即空指针调用崩溃。**同一函数内三个回调两种口径**，这条不一致本身就是可判定的依据。
- 复现步骤: 用 `read_dsb_bars(path, NULL, NULL, NULL)` 调一个内容为空的合法文件即可命中 L755。
  本轮未启用该用例（会崩进程、且需要真实块格式夹具），登记为待验证输入。
- 影响范围: 任何「只关心有没有数据、不要数据本体」的调用方都会踩；与 BUG-L05-007 组合时，
  调用方连「先判空再决定传哪些回调」的机会都没有。
- 备注: 修复方向（待评审）：`if(cbCnt) cbCnt(...)`、`if(cb) cb(...)`，或把参数改成带默认值的可空回调并统一守卫。
