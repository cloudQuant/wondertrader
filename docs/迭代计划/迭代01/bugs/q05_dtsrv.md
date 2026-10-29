# q05_dtsrv 缺陷清单（迭代01 第五波 · WtDtCore/WtDtServo/WtDtHelper）

- 范围: 本波缺陷逐项复核；未修改任何生产代码或测试。
- 关联用例目录: `src/TestUnits/coverage/q05_dtsrv/`

## BUG-Q05-001 WtDtServo::WtDataManager::initStore 模块加载失败后空指针解引用
- 状态: 静态缺陷成立；DISABLED 崩溃用例未运行
- 严重级别: Blocker
- 位置: `src/WtDtServo/WtDataManager.cpp:59-88`
- 现象: `DLLHelper::load_library` 失败（或插件缺 `createRdmDtReader`/`deleteRdmDtReader` 符号）时 `_reader` 保持 NULL，但 L87 无条件执行 `_reader->init(cfg, this)`，直接段错误。`init()` L104 又无条件调用 `initStore(cfg->get("store"))`，即「配置里 store.module 写错 / 插件没装」这一最常见部署故障路径必崩，而不是返回 false。
- 对照: 兄弟实现 `src/WtDtCore/DataManager.cpp:67-107` 同形状代码在加载失败时 `return false`，且 `_writer->init` 在守卫之后——Servo 版疑似移植时丢了这个 return。
- 复现: `q05_servo_wtdatamgr_test.cpp` 的 `DISABLED_Q05ServoWtDataManagerTest.DISABLED_InitStoreLoadFailureDerefsNullReader`（按硬约束保持禁用，启用即段错误）。

## BUG-Q05-002 ParserAdapter/ParserAdapterMgr 在当前 coverage 配置下重复链接
- 状态: 已撤回（当前 CMake 将相关 TU 拆成独立测试目标）
- 严重级别: 不适用（当前目标隔离后不存在该链接问题）
- 位置: `src/WtDtCore/ParserAdapter.{h,cpp}` 与 `src/WtDtServo/ParserAdapter.{h,cpp}`
- 现象: 两类同名且形状不同，若编进同一二进制会有类型/符号冲突；但当前 `src/TestUnits/coverage/CMakeLists.txt` 已把 WtDtCore 与 WtDtServo 分别编成 `CoverageTests_dtcore`、`CoverageTests_dtservo`，并注释说明不能放在同一目标。没有证据支持当前 coverage 配置链接失败。
- 处置: 这是针对旧目标假设的误报，状态为**已撤回**。当前目标拆分不需再改 CMake，也不能据此要求生产类加命名空间。

## BUG-Q05-003 IndexWorkerMgr::handle_quote 同步分支 retain 后不 release（行情对象泄漏）
- 状态: 已合并（BUG-R18-007；同一源码位置与同一 retain/release 失衡）
- 严重级别: Major
- 位置: `src/WtDtCore/IndexWorkerMgr.cpp:170-203`
- 现象: L180 `newTick->retain()` 之后，仅 `_pool` 异步分支在任务末尾 `newTick->release()`；`_pool==NULL` 的同步分支无配对 release。与 BUG-R18-007 完全重复，不另计数。
- 佐证: 两分支唯一差异就是这句 release；语义上 handle_quote 对入参引用计数应平衡。
- 复现: `q05_idxworker_mgr_test.cpp::HandleQuoteNullUnsubscribedAndSubscribed` 走过部分同步路径，但未断言引用计数，也未测长期池增长。按 BUG-R18-007 合并处理。

## BUG-Q05-004 resample_bars 对 cb/cbCnt 无 NULL 守卫（cbLogger 有）
- 迭代02第二轮前状态（历史）: - 状态: 待验证（空回调是否违背调用合同未确认；零输出可达性未证）
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Minor
- 位置: `src/WtDtHelper/WtDtHelper.cpp:1021-1023`
- 现象: 成功路径裸调用 `cbCnt`/`cb`；头文件仅把 `cbLogger` 设为可选，`cb`/`cbCnt` 没有默认值，故源码证明传 NULL 会崩，但尚无证据说明 NULL 是合法调用。`newCnt==0` 时访问 vector 首元素的静态风险成立，但 `resample_bars` 传入 `bIncludeOpen=true`，本轮没有证明该路径能产生零输出。
- 复现: 未写启用用例（属崩溃类输入，与 BUG-Q05-001 不同类，暂不建 DISABLED 条目，交排除清单评审）。

## BUG-Q05-005 StateMonitor::initialize 的配置泄漏与会话 ID 缓冲区溢出风险
- 状态: 混合项；配置引用未释放是静态缺陷，其它子项的输入合同单独列明
- 严重级别: Minor
- 位置: `src/WtDtCore/StateMonitor.cpp:49-74`、`src/WtDtCore/StateMonitor.h:47`
- 现象: ① **状态：静态资源缺陷成立。** `StateMonitor::initialize` 成功读取的 `config` 在函数末尾未 `release()`，配置树未见其它 owner。② **状态：待验证。** `StateInfo::_session` 为 `char[16]`，`strcpy` 写入会话 JSON key；超过 15 字符可越界，但允许的 session id 最大长度未找到合同。③ **状态：静态异常输入泄漏成立。** `WTSBaseDataMgr::loadSessions` 对缺失/非数组 `sections` 的条目，在 `continue` 前已创建 `WTSSessionInfo` 且未 release；是否接受并跳过此类配置行需在接口合同中注明。
- 进一步复核: `src/WTSTools/WTSBaseDataMgr.cpp:265-293` 在正常条目上调用 `_session_map->add(id, sInfo)`；该 `WTSHashMap` 的 `add` 默认 retain（`src/Includes/WTSCollection.hpp:660-663`），随后也未释放创建者的初始引用。因此成功路径同样存在引用不配对的静态泄漏风险，不仅限于异常行。本次没有运行引用计数或泄漏检测。修复时应统一创建、入表与异常退出的所有权，并覆盖成功和缺失 sections 两条路径。
- 备注: session ID 长度子项未动态验证；本轮不运行越界输入。

## BUG-Q05-006 resample_bars 对缺失 sections 先索引再校验，触发 RapidJSON assertion
- 状态: 已确认（本机单项运行 exit 134；未使用 sanitizer）
- 严重级别: Major（外部 session JSON 缺字段时进程 abort）
- 位置: `src/WtDtHelper/WtDtHelper.cpp:900-922`（解析 session JSON）
- 现象: 代码先执行 `const rj::Value& jSecs = root["sections"]`，之后才检查 `jSecs.IsNull() || !jSecs.IsArray()`。成员完全缺失时 RapidJSON `operator[]` 先触发断言，预期的格式错误日志和 `return 0` 分支不可达。`BadSessionTemplateRejected` 已提供 `{"offset":0,"auction":null}` 的缺字段输入。
- 修复方向: 先校验 `root.IsObject()`、`HasMember("offset")`/`HasMember("sections")` 和字段类型，再访问字段；确认 required/optional 字段合同。会话对象最好在校验完成后创建，或用 RAII 确保错误分支 release。
- 证据边界: 仅本机 `CoverageTests_dtcore` 单项运行确认 abort；未用 ASAN/UBSAN，未在其它平台复测。

## 待裁定（接口合同或 oracle 尚不充分）
1. `Q05ResampleTest.DayResampleOfTenDailyBarsPassesThrough` 断言 `d→d(times=1)` 全区间透传 10 条；实测返回 0。`WTSDataFactory::extractKlineData` 在 `times<=1` 时明确返回 NULL，`resample_bars` 因而返回 0；但公开注释未明确 `times` 必须大于 1，接口是否应支持 no-op 请求仍待合同裁定，不更改期望。
2. `MinuteResampleReportsConsistentCounts` 只断言返回值、计数 callback、数据 callback 自一致，不断言 m5 具体条数；目前没有唯一条数 oracle。
3. `Q05StateMonitorTest.InitializeMalformedJsonReturnsFalse` 本机过滤运行 PASS，已确认当前坏 JSON 路径返回 false；原有 loader 疑问撤销。

## 2026-10-29 独立复核与本机测试记录

本轮复核源码、接口与现有测试定义；结果来自本机现有 coverage 二进制，没有声称 sanitizer、其它平台或生产运行证据。

### 逐项分类

- **BUG-Q05-001 — 静态缺陷成立。** `initStore` 在模块加载失败或必要符号缺失时保留 `_reader == NULL`，随后无条件调用 `_reader->init`。修复方向：对加载、符号、creator 和 `_reader->init` 逐步判错并返回 false，失败时释放已打开模块。禁用用例本轮未执行；结论来自源码控制流，不是崩溃动态复现。
- **BUG-Q05-002 — 已撤回。** 当前 CMake 分别构建 `CoverageTests_dtcore` 和 `CoverageTests_dtservo`；文件注释明确说明两种 `ParserAdapter` 不能混在同一目标。原 `CoverageTests_dtsrv` 强符号重复前提不成立。
- **BUG-Q05-003 — 已合并（BUG-R18-007）。** 同一行同步 retain 没有 release；当前测试虽执行部分同步分支，但没有引用计数断言，BUG-R18-007 是主记录。修复方向以主记录为准：同步路径配对释放或使用作用域守卫。
- **BUG-Q05-004 — 待验证。** 回调裸调用是源码事实；头文件只明确 `cbLogger` 可选、未声明 `cb/cbCnt` 可空，因此 NULL 输入是否违约未确定。`newCnt==0` 未证明可达。修复方向：明确必填回调合同并在入口拒绝 NULL，或统一支持可选回调并守卫调用；若保留零输出路径，先检查 size 再取元素。
- **BUG-Q05-005 — 子项拆分。** `config` 未 release 为静态资源缺陷成立，未做泄漏测量；修复方向为 RAII/统一 release。session ID 长度合同未证，溢出子项待验证；可按容量检查拒绝过长 id 或改用长度受控存储。`loadSessions` 缺 `sections` 时先创建对象再 continue、未 release 是静态异常输入泄漏点；在跳过前 release，或先验证结构再创建对象。
- **BUG-Q05-006 — 本机动态确认。** 缺失 `sections` 触发 RapidJSON assertion；测试对错误输入期待返回 0 的前提与源码中显式格式检查相符，但访问顺序让检查不可达。建议 `HasMember`/类型校验后再建会话对象。

### 本机目标与结果

- 历史版本运行 `build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_dtcore --gtest_filter='Q05*.*-Q05ResampleTest.BadSessionTemplateRejected'`：25 项，22 PASS、3 FAIL。其中 `Q05IdxWorkerMgrTest.WrapperOwnsAndReturnsDeleteToFactory` 旧断言使用了**未 init 的 manager**；当前测试已先用空 indice 配置调用 `mgr.init(cfg,&hot,&bd,&dm)`，再检查实际传入的 manager。当前版本独立运行该单例 PASS（`LLVM_PROFILE_FILE=/tmp/wt-review-q05-%p.profraw`）；历史 3 FAIL 的其余两项仍见下文，不能用此单例结果替代整组重跑。未初始化 manager 的产品风险已归并至 BUG-R44-007。
- 同一目标的 `Q05ResampleTest.MissingOrShortFileFailsHeadCheck` 收到 `std::runtime_error("File does not exist")`；这是 BUG-L05-007 `StdFile::read_file_content` 缺文件抛异常口径的重复，**状态：已合并（BUG-L05-007）**。此项不是新 Q05 缺陷。
- 同一目标 `Q05ResampleTest.DayResampleOfTenDailyBarsPassesThrough` 失败为 0 vs 10；见上方待裁定，未擅自改期望。
- 单项运行 `CoverageTests_dtcore --gtest_filter='Q05ResampleTest.BadSessionTemplateRejected'` 进程 exit 134，打印 RapidJSON `document.h:1231` assertion；这是 BUG-Q05-006 本机证据。本次没有运行 sanitizer。
- `build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_dtservo --gtest_filter='Q05ServoWtDataManagerTest.*'`：3 PASS、1 DISABLED。
