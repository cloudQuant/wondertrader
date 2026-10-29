# R8 缺陷清单（迭代01 第七波 · WtDtServo/WtDtRunner.cpp + WtDataManager.cpp + ParserAdapter.cpp）

- 范围: 本 lane 仅记录，未修改任何生产代码；`src/**`（除 `src/TestUnits/coverage/q05b_dtservo/r8_*.cpp`）一字未动。
- 关联用例: `r8_parseradapter_test.cpp`（13 条全启用）、`r8_wtdtrunner_test.cpp`（13 启用 + 3 DISABLED）、`r8_wtdatamgr_sinks_test.cpp`（6 启用 + 2 DISABLED）
- 关联既有记录: BUG-Q05-001（`q05_servo_wtdatamgr_test.cpp::DISABLED_InitStoreLoadFailureDerefsNullReader`）已登记 initStore 空 reader 解引用；R8-002 是默认模块名下的同根因变体，合并到 Q05-001。R8-003 另核对委托入口和 reader 未初始化时的调用路径。
- 同名类隔离: 本文件全部行号指向 `src/WtDtServo/`，与 `src/WtDtCore/ParserAdapter.h:28-30`（3 参构造）不是同一个类，未引用 WtDtCore 的任何符号。

## 本次审查边界（2026-10-29）

- 核对了源码、测试前置条件及头文件生命周期说明；**本次没有运行测试**。测试名/断言只代表源码记录，不是本次执行证据。
- 源码问题：R8-001 已确认；R8-003 的空 reader 委托崩溃路径已确认，但“452 行产品不可达”的说法撤回为覆盖缺口；R8-006 的②/③确认、①撤回。
- 已合并：R8-002 并入 BUG-Q05-001。
- 已撤回：R8-004 的正常产品调用违反 `initialize` 前置条件；R8-005 的“缺 commodity 仍可从标准装载器取得 contract”前提不成立；R8-007 的热重载/适配器累积链被 `_is_inited` 一次性守卫阻断。

## BUG-R8-001 initialize 未判 basefiles 段，缺段即空指针解引用
- 状态: 已确认（源码静态核实；DISABLED 崩溃用例本次未运行）
- 严重级别: Blocker
- 位置: `src/WtDtServo/WtDtRunner.cpp:73-74`
- 现象: `WTSVariant* cfgBF = config->get("basefiles");` 之后直接 `if (cfgBF->get("session"))`，中间没有判空。`WTSVariant::get` 未命中返回 NULL 是本文件的既有语义（同函数 `:152-153` 对 parsers、`WtDataManager.cpp:104` 对 store 都是靠这个返回值判空的），所以任何一份漏写 `basefiles` 的 wtdtserver 配置（或把段名写成 `basefile`/`bases`）都会在 `initialize()` 的第一条成员访问上 SIGSEGV，且崩溃发生在 `WTSLogger::init` 之后，日志里连一行 error 都不会有。
- 影响范围: 数据服务启动路径的唯一入口，配置文件笔误 = 进程直接段错误退出而不是「Loading config failed」那样的可诊断早退（对照 `:59-64` 的处理口径，同函数内前后不一致）。
- 复现: `r8_wtdtrunner_test.cpp::DISABLED_InitializeWithoutBasefilesSectionSegfaults`（根配置只含 `disable_dump`，触发条件：启用该用例即崩，禁止在 CI 打开）。

## BUG-R8-002 initStore 加载失败后无条件解引用 _reader（默认模块名形态）
- 状态: 已合并（默认模块名变体并入 BUG-Q05-001）
- 严重级别: Blocker
- 位置: `src/WtDtServo/WtDataManager.cpp:54-56`、`:81-88`
- 现象: `module` 为空时回落到默认名 `WtDataStorage`；若该插件不存在，或缺 `createRdmDtReader`/`deleteRdmDtReader` 任一符号，`_reader` 保持 NULL 后在 `:87` 解引用。此时不是“返回 true”：进程在 `initStore` 返回前崩溃；调用方确实拿不到可处理的失败结果。
- 影响范围: 与 BUG-Q05-001 完全同根因；本条只记录 `store:{}`、默认模块名这一配置变体，不作为独立发现。
- 证据界限: `r8_wtdatamgr_sinks_test.cpp` 将模块目录钉到空临时目录，并保留 DISABLED 崩溃用例；本次未运行。静态调用链与 Q05-001 相同。
- 修复方向: 在创建/解析导出函数失败时返回 false 并保持 `_reader` 空值安全；由 `WtDtRunner` 传播初始化失败。

## BUG-R8-003 WtDataManager 空 reader 委托入口零判空；覆盖量不是产品不可达
- 状态: 已确认（空 reader 委托路径有崩溃风险；行数仅作当前测试覆盖缺口）
- 严重级别: Major
- 位置: `src/WtDtServo/WtDataManager.cpp:116`、`:121`、`:128`、`:135`、`:142`、`:170`、`:203`、`:215`、`:247`、`:341`、`:357`、`:398`、`:487`、`:496`、`:516`、`:543`
- 现象: 除 `clear_cache`（`:609-612` 判空）外，取数/订阅入口直接调用 `_reader->xxx()`。`data` 段缺失时 `initDataMgr` 直接返回；有 `data` 但没有 `store` 时 `WtDataManager::init` 返回 false。runner 都不检查该结果，仍在 `:189` 将 `_is_inited` 设为 true；随后查询入口通过自身守卫并到达空指针委托。该空 reader 安全问题与 Q05-001 属同一失效状态，但无 `data/store` 的初始化路径尚未在 Q05-001 中单独展开。
- 影响范围: 文中列出的 452 行是当时测试统计出的未覆盖范围，不是产品运行不可达；合法插件可提供 reader。当前没有 reader 注入接缝，且真实 `WtRdmDtReader::init` 会起巡检线程，因此测试夹具选择了 DISABLED 崩溃用例，形成覆盖缺口。
- 证据界限: 查询入口和 `_is_inited`/init 返回值处理可静态核对；DISABLED 测试源码包含空 reader 路径，本次未运行。`IRdmDtReader` 是纯虚接口，但 `WtDataManager::_reader` 为私有且由 DLL factory 创建，测试替身需经受控 factory/插件构建，不能把“缺少简单 setter”说成绝对不可测。
- 修复方向: 把 `WtDataManager::init` 失败传回 runner 并阻止 runner 标记成功；在 reader 未初始化时让查询/订阅入口返回失败值，或将该状态写成明确前置条件。覆盖验证可用不启线程的 fake reader 插件。

## BUG-R8-004 WtDtRunner 构造函数漏初始化两个回调指针
- 状态: 已撤回（报告调用顺序违反 WtDtRunner 的公开生命周期前置条件）
- 严重级别: Major
- 位置: `src/WtDtServo/WtDtRunner.cpp:28-32`（初始化列表只有 `_data_store`、`_is_inited`），消费点 `:463`、`:474`、`:481`、`:490`、`:501`、`:639-642`
- 现象: `_cb_tick` / `_cb_bar` 是未初始化的成员函数指针，`initialize()` 在 `:52-53` 赋值。只有绕过头文件明确要求的 initialize-before-use 生命周期、直接在默认构造后调用低层方法时才会读到未初始化值；WtDtServo C 导出层先调用 `initialize`，没有“顺序写反”的证据。
- 影响范围: `_cb_tick/_cb_bar` 确实未在构造函数中初始化，但头文件明确要求先 `initialize` 再调用其他方法；`initialize` 在配置装载前即赋值两个回调。当前正常 parser→runner 路径先初始化，不支持以违反前置条件的直接调用认定产品缺陷。
- 证据: 初始化顺序来自 `WtDtRunner.cpp:43-53`，公开生命周期说明来自 `WtDtRunner.h:55-68`；本次未运行测试。
- 修复方向: 如需支持未初始化对象上的低层方法调用，再补默认空指针初始化和守卫；本条不作为已确认缺陷。

## BUG-R8-005 proc_tick 对 commInfo 零判空，合约未挂品种时段错误
- 状态: 已撤回（“缺 commodity 仍成功加载 contract”的前置条件被标准装载器排除）
- 严重级别: Major
- 位置: `src/WtDtServo/WtDtRunner.cpp:422-428`
- 源码核对: `proc_tick` 对非空 `cInfo` 确实直接使用 `getCommInfo()`；但 `WTSBaseDataMgr::loadContracts` 在 L422-467 找不到 commodity 时会 `continue`，不会登记该 contract。因此所述“仅因 commodities 配置漏项，contract 仍能由标准 `getContract` 命中”的前置条件与标准装载器冲突。
- 剩余边界: `WTSContractInfo` 可被手工创建并附到 tick，但本记录没有证明 parser/runner 的受支持输入会提供 `commInfo == NULL` 的 contract；若要覆盖外部手工注入，应另立并验证接口契约。
- 证据: 装载器分支和 `getCommInfo` 使用点静态可核对；没有对应运行测试，本次也未运行测试。

## BUG-R8-006 ParserAdapter::init 的死分支、拒绝态污染 _id 与 _cfg 引用泄漏
- 状态: 部分确认（①已撤回；②/③已确认，源码静态核实；本次未运行测试）
- 严重级别: Minor
- 位置: `src/WtDtServo/ParserAdapter.cpp:128-132`、`:149` 与 `:197-200`；`:87-91`；`:92-93` 与 `:205-217`
- 现象: ① `:128-132` 的 factory 返回 NULL 会先 `return false`，因此从该路径无法到达 `:149`；后续 `:197-200` 的 else 分支不可达，`:52` 的 `initExt` 判空后赋值再检查也是恒真。但这属于无效分支，不是产品故障，撤回为可选清理。② `_id = id` 在 `_cfg != NULL` 拒绝之前，失败的重复 init 会改写 `id()`，而 `_parser_api/_cfg` 仍属于首次 init。对应测试源码明确断言了 `pa_second`，该断言记录的是现状，不是正确性契约。③ `:93` retain `_cfg` 后，`release()`（`:205-217`）和空析构（ParserAdapter.cpp:40-42）均不 release；`ParserAdapterMgr::release()` 只调 adapter 的 `release()`，仍不释放 `_cfg`。`WtDtRunner` 析构也不调用 manager 的 release，因此 runner 释放时 adapter 的 cfg 引用泄漏。
- 影响范围: ② 失败重试会使 id 与首次绑定状态不一致，可能误导按 id 的日志/排障；③ 每个执行到 retain 的 adapter 都可能留下 config 引用。未核实 DLL handle 的生命周期，本条不主张 DllHandle 泄漏。
- 证据界限: `InitWithEmptyModuleRejectsButStillBindsIdAndCfg` 源码包含②的断言；①/③由控制流和 retain/release 配对静态核对。本次未运行测试，也未做分配器泄漏检测。
- 修复方向: ②拒绝重复 init 前不要改 `_id`；③在 adapter 生命周期终点配对释放 `_cfg`；①仅作死代码清理，不登记产品缺陷。

## BUG-R8-007 initParsers 的自动 id 是函数级静态，跨实例只增不减
- 状态: 已撤回（报告中的同 runner 热重载/旧 adapter 累积被一次性初始化守卫阻断）
- 严重级别: Minor
- 位置: `src/WtDtServo/WtDtRunner.cpp:390-395`（`static uint32_t auto_parserid = 1000;`）
- 源码核对: 同一 `WtDtRunner` 第二次 `initialize` 在 L46-50 直接返回；`_is_inited` 在第一次完整流程末尾 L189 置 true，所以热重载不会再次运行 `initParsers`，不存在该路径上的 adapter 累积。不同 runner 实例各自持有独立的 `ParserAdapterMgr`，函数静态计数器产生不同 id 不会触发同一个 map 的重复检测。
- 影响范围: 静态计数器跨实例递增是当前实现事实，但此处没有证据表明它造成日志/订阅错误；测试源码只验证无 id 配置可登记，本次未运行。
- 修复方向: 若产品要求跨 runner 的 id 可复现，应作为明确的命名契约另行评审；不作为已确认缺陷。
