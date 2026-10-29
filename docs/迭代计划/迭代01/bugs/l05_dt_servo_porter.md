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
- 相关测试: `l05_dt_helper_bar_test.cpp: L05HelperBarTest.StoreBarsReportsMissingParentDirectoryFailure`（已将历史的错误成功断言改为期望返回 false；当前定向运行 FAIL）
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

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
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

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Major
- 模块/文件: WtDtPorter / `src/WtDtPorter/WtDtPorter.cpp` `write_log()`（L83-93）
- 相关测试: `l05_dt_porter_capi_test.cpp: WriteLogWithAndWithoutCategory`（只覆盖 `""` 与非空分类，未用 NULL 触发崩溃）
- 输入/前置条件: 外部调用 `write_log(1, "msg", NULL)`
- 期望行为: 导出 C 边界应把 `catName == NULL` 当作「无分类」处理
- 实际行为: 无条件 `strlen(catName)` → 空指针解引用崩溃
- 复现步骤: 任意语言绑定漏传 catName（默认参数为 NULL）即可复现
- 影响范围: 所有使用 Porter 日志回调的解析器/收割器插件
- 备注: 同类问题：`WtDtServo.cpp` 的 `get_*` 系列在 `kData != NULL` 时无条件调用 `cbCnt(...)`/`cb(...)`，未校验回调是否为 NULL。建议补 NULL 防护。


- 迭代02复核: TEST。write_log 对 catName strlen；公开注释只承诺空字符串表示默认分类，没有承诺 null 等价空串，候选输入超出契约。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
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

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
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


- 迭代02复核: TEST。cb/cbCnt 无默认值，文档说读取先通知数量再传数据；只有 cbLogger 明确可选。直接空回调违反现合同。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## 独立逐项复核（2026-10-29）

本节依据当前源码、导出头文件和测试定义做静态核对；本次没有运行测试或编译。测试中的断言只记录其写下的期望，不作本轮动态复现证据。

### BUG-L05-001：源码越界访问成立；两段日期不是已验证的有效输入

`strToDate` 对 `ay.size() > 1` 便访问 `ay[2]`（`src/WtDtHelper/WtDtHelper.cpp:148-160`），两段输入会越界；CSV 的日期字段格式是否接受 `YYYY/MM` 尚无契约证据，但无效/截断日期也不应造成 UB。现有 `TransCsvBarsDailyRoundtrip` 只给 `2020/01/16` 三段日期和 `20200117` 单段日期（`src/TestUnits/coverage/l05_dt_servo_porter/l05_dt_helper_bar_test.cpp:271-274`），没有两段边界用例，本次未运行。建议明确接受的日期格式，按段数和日历值校验，拒绝非 1 段紧凑日期及非 3 段分隔日期。

### BUG-L05-002：失败仍报成功为源码确认；历史测试曾把错误现状写成期望

五个 `store_*` 仅在 `create_new_file` 成功时写数据，却不检查创建及 `write_file` 结果，随后无条件记录成功并返回 `true`（`WtDtHelper.cpp:1077-1086,1116-1126,1156-1166,1196-1206,1236-1246`）。`BoostFile::create_new_file` 在句柄无效时返回 `false`，`write_file` 也返回布尔值（`src/Share/BoostFile.hpp:44-50,147-154`）。`trans_csv_bars` 则在建文件失败后仍调用 `write_file(cmprsData)`，忽略写入结果并继续报“已转储”（`WtDtHelper.cpp:464-472`）。这是文件路径不可写/落盘失败时的可确认假成功；实际磁盘损坏等其它失败形态本次未测。历史测试 `StoreBarsSwallowsFileCreationFailure` 曾明确期待“返回 true 且无文件”，该错误 oracle 已改为失败时应返回 `false`，不是产品契约证据。建议校验创建与每次写入结果；失败时 `store_*` 返回 `false`，CSV 转换记录错误并跳过后续成功日志。

后续验证（2026-10-29）：旧测试已改名为 `L05HelperBarTest.StoreBarsReportsMissingParentDirectoryFailure`，先确认父目录不存在，再断言存储失败且目标文件不存在。重新构建后单跑 FAIL：`store_bars` 实际返回 true，期望 false。

### BUG-L05-003：压缩块错误处理缺陷确认；空指针属于无效调用边界，原条目调用点计数不完整

`proc_block_data` 在压缩块 `_size` 与文件长度不符时返回 `false` 且不改动 `content`（`WtDtHelper.cpp:52-60`）；调用方忽略结果后继续按结构体大小计数/回调。源码共有八处调用（`WtDtHelper.cpp:215,309,611,646,681,716,750,939`），原清单列的七处漏了 `resample_bars`。因此损坏块可被当作未解压内容解析；`dump_ticks` 仅按扩展名筛文件而未核对块类型（`:290-311`），把非 tick `.dsb` 当 tick 读取也属条件性错误。现有 header-validation/roundtrip 用例未构造 `_size` 不匹配的块，本次未运行。建议所有调用点在 `false` 时停止当前文件处理并报告错误，验证块类型和长度后再交数据。另，`store_*` 在 `count>0 && firstItem==NULL` 时会把无效指针传给 `memcpy`（如 `:1060-1063,1098-1101`）；这是源码可确认的 UB，但指针是数据输入且头文件将其描述为数据数组首地址（`WtDtHelper.h:218-241`），空指针不是已确认的合法调用。建议入口拒绝负数/正数配空指针；不能把它表述为普通有效调用“稳定崩溃”。

### BUG-L05-004：头文件单独使用存在未解析类型；产品运行影响尚未建立

`StatHelper.hpp` 使用 `BoostWriteLock`、`BoostReadLock`、`BoostRWMutex`，头文件本身未声明这些别名（`:91,118,124`），源码搜索只找到该头文件中的引用；测试为编译它而在测试 TU 临时 typedef 三个别名并抑制未使用变量告警（`l05_dt_core_util_test.cpp:15-34`）。这支持“独立包含不可用”的静态判断，但本次没有实际执行独立 TU 编译；仓库运行路径也没有包含或调用 `StatHelper`，因此不能据此称现有广播运行时失效。`flag` 计算后未读取为源码事实，是否触发告警取决于编译器/告警选项；溢出时将计数置为增量的语义仍需产品定义。建议补齐自包含依赖/锁类型、删除或实际使用 `flag`，并明确溢出策略；若该头文件不再支持，应从对外可用面移除，而非只补测试别名。

### BUG-L05-005：`write_log(NULL)` 的 UB 仅在调用方传空指针时成立；原记录误写了默认值

实现对 `catName` 调 `strlen`（`src/WtDtPorter/WtDtPorter.cpp:83-92`），故显式传 `NULL` 有未定义行为；但公开声明没有 `= NULL` 默认值，头文件只说明“空字符串表示默认类别”（`src/WtDtPorter/WtDtPorter.h:45-52`），没有承诺 NULL 可用。现有测试仅传非空串和空串（`src/TestUnits/coverage/l05_dt_servo_porter/l05_dt_porter_capi_test.cpp:80-85`），没有 NULL 调用，本次未运行。此处与 `BUG-R22-008` 是同一导出行的重复记录；R13/Q06 中相似的 `write_log` 属其它模块入口，不等于同一代码位置。建议定义 NULL 语义：若类别必填则拒绝并记录错误；若等同默认类别则先判空再转空串。原记录的“默认参数为 NULL”应以本复核为准。

原备注还把 `WtDtServo::get_*` 的 `cb`/`cbCnt` 缺少守卫并入 L05-005。那些调用点确实无判空（`src/WtDtServo/WtDtServo.cpp:76-87,96-108,116-132,140-152,160-176,184-200,208-220`），但 `WtDtServo.h` 没有默认值，接口文档描述会通过两类回调返回数据（`:57-129`）；目前没有证据表明 NULL 是允许输入。应作为单独的回调可空性契约问题记录，不能据此扩大 `write_log` 的结论。

### BUG-L05-006：错误日志参数已由源码确认；现有测试只验消息类别

`dump_bars` 在头部校验失败和类型不匹配时均把 `binFolder` 写入日志，而循环中已有具体 `path`（`WtDtHelper.cpp:188-200,204-210`）；`dump_ticks` 同样记录目录而非当前文件（`:293-305`）。该项属于诊断信息不准确，不影响转换结果。当前 bar/tick 测试验证出现“头部校验失败”等文字，不验证其中路径指向具体坏文件（`l05_dt_helper_bar_test.cpp:210-256`、`l05_dt_helper_tick_test.cpp:325-343`）。建议以当前迭代器的文件路径记录日志；路径断言可作为回归证据，本次未运行。

### BUG-L05-007：缺文件路径会抛异常，返回值/日志期望与实现冲突；异常是否允许需由接口契约定案

`StdFile::read_file_content` 对不存在路径主动抛 `std::runtime_error`（`src/Share/StdUtils.hpp:57-63`），所以 `read_dsb_*`/`read_dmb_*` 在读取前不会走各自的“头部校验失败→0”分支（如 `WtDtHelper.cpp:735-747,769-779`）。头文件把读取函数描述为返回条数、并把 `cbLogger` 标为可选，但没有写异常契约；关联启用测试则明确期待缺文件返回 0（`l05_dt_helper_bar_test.cpp:194-201`、`l05_dt_helper_tick_test.cpp:224,282`）。这是源码与测试期望的冲突；本次没有运行测试，不能称这些用例本轮“红”。函数是导出的 C++ 函数，不能仅因形状像 C API 就断言异常越过 `extern "C"`。建议选择并统一契约：在读取边界捕获文件错误、记日志并返回 0，或明确文档化可能抛出的异常并更新测试和所有调用方。原记录关于 `-fexceptions` 关闭后“直接终止”的表述不成立为运行时证据；禁用异常通常会影响编译，不应作为本条影响结论。

### BUG-L05-008：空回调时会调用空函数指针为源码事实；NULL 是否违约尚未定义

`read_dsb_bars` 在空内容分支无条件调用 `cbCnt(0)`，数据分支无条件调用 `cbCnt` 与 `cb`（`WtDtHelper.cpp:752-761`）；其它 `read_*` 也有同类写法。公开头文件没有给 `cb`/`cbCnt` 默认值，并描述这两类回调承担返回数据/计数通知（`WtDtHelper.h:130-187`），而只明确把 `cbLogger` 设为可选。因此源码并不支持“只关心数量、不提供数据回调”是当前合法调用的说法；现有测试均传入 `cb`/`cbCnt`，本次未运行。建议在接口文档明确它们是必填并在入口拒绝 NULL，或承诺可选后统一守卫并增加隔离测试。`BUG-Q05-004` 是 `resample_bars` 的相似模式但落点不同，不与本项合并为同一源码 occurrence。
