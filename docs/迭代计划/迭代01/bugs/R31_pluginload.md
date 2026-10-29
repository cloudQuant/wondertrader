# R31_pluginload（WtCore 插件装载成功支路）复核记录

> 状态一律「待审核」；本迭代不修改 `src/**`。
> 原始 lane 登记三条候选；本复核区分源码推导与实际插件运行证据，
> 前两条是同一族 `loadFactories` 的入口符号约定问题，第三条是同一函数里的 NULL 契约不一致。

## BUG-R31-001 SEL manager 与仓库 SEL factory 入口符号名不匹配（真实模块运行未覆盖）

- 状态: 待审核
- 严重级别: 源码确认接口名不一致；实际 SEL 模块加载结果未动态验证
- 模块/文件: `src/WtCore/SelStrategyMgr.cpp`（`loadFactories` L61、L75）；对照 `src/WtSelStraFact/WtSelStraFact.cpp`（L28、L38）
- 相关测试: `src/TestUnits/coverage/q01_wtcore3/r31_stra_mgr_pluginload.cpp`
  → `r31_stra_mgr.no_product_plugin_exports_the_sel_entry_symbol`（直接钉住这条事实）
  → `r31_stra_mgr.sel_load_of_plugin_like_dir_always_misses_its_entry_symbol`（走 L61-66 的必然失败支）
- 输入/前置条件: 仓库源码的 SEL factory 与 `SelStrategyMgr` 存在装载关系。源码搜索 `createMfStrategyFact` 只命中
  `SelStrategyMgr.cpp:61` 一处 —— 当前源码未见模块导出这个名字；
  而 SEL factory `src/WtSelStraFact/WtSelStraFact.cpp:28` 导出的是 `createSelStrategyFact`
  （删除口同理：`deleteSelStrategyFact` vs 代码找的 `deleteMfStrategyFact`，L75）。
  原 lane 记录称逐个 `dlopen` 过本机 12 份产物（CTA/TwoMa/ExeFact/RiskMon/IndexFactory/DataStorage(AD)/
  MsgQue/ShareHelper/UftStraFact/Porter 子目录），`createMfStrategyFact` 命中数为 0；这些列表不含真实 SEL factory，故不能替代真实 SEL 模块验证。
- 期望行为: `SelStrategyMgr::loadFactories` 找到的入口符号与 SEL 插件导出的符号一致，
  选股策略插件能被登记进 `_factories`，`createStrategy`/`getStrategy` 可用。
- 源码推导（若装入模块未导出 manager 所需符号）: `get_symbol(hInst, "createMfStrategyFact")` 返回 NULL ⇒ 该模块走
  `DLLHelper::free_library(hInst); continue;`（L62-66），`count` 恒 0，函数仍返回 true。
  调用方看不出装载失败（`src/WtPorter/WtPorter.cpp:364 reg_sel_factories` 只是转发这个返回值）。
  原 lane 将若干后续成功/创建行计为未覆盖；当前证据不足以把它们称为结构性死码，因为外部插件可能采用 manager 所需的导出契约。
- 复现步骤: `ctest`/手工跑 `CoverageTests_wtcore --gtest_filter='r31_stra_mgr.no_product_plugin_exports_the_sel_entry_symbol'`；
  或 `nm -gU build-cov/build_x64/Debug/bin/libWtSelStraFact.dylib`（本机连这份产物都没有，
  因为 `src/WtSelStraFact` 不在顶层 `ADD_SUBDIRECTORY` 清单里，见 `src/CMakeLists.txt:246-313`）。
- 复核: 名称不一致是源码可确认的问题候选；本机构建目录没有 `libWtSelStraFact.dylib`，测试目录也不等于真实 SEL 插件。因此只能说“若将仓库 SEL factory 交给该 manager，入口查找会失败”，不能声称所有第三方 SEL 插件或实际构建产品均已动态验证为不可用。删除接口还有 ABI 签名差异：`Includes/SelStrategyDefs.h` 声明 `ISelStrategyFact*&`，仓库导出的删除函数接收 `ISelStrategyFact*`；改名时要统一完整函数签名。修复前确认公开插件契约；启用测试应装载真实 SEL 模块并验证注册、创建和释放。
- 影响范围（条件性）: 若生产 SEL 工厂采用仓库当前导出名并交给 manager，插件注册链路会失败；其他第三方插件是否采用 manager 所需符号未知。
  原始影响范围主张为选股（SEL）策略的插件化机制整体不可用 —— 用户把选股插件放进
  `sel.stra.folder` 也一个都装不上，且没有任何日志提示符号名不对（只有 `0 … loaded` 的计数）。
  `SelStraContext`/`SelEngine` 侧靠插件提供策略的链路因此全部不可测。
- 备注: 修复方向二选一 —— 把 L61/L75 改成 `createSelStrategyFact/deleteSelStrategyFact`
  （与插件现状一致，改 2 行），或把插件导出改成 `createMfStrategyFact`（改 2 行 + 波及
  `src/WtSelStraFact` 的既有使用方）。同时建议 `loadFactories` 在「目录里有 `.dylib`
  但入口符号全找不到」时记一条 warn，否则这类约定漂移每次都只能靠人肉发现。
  另注：`src/WtSelStraFact` 不在顶层构建里，属构建范围决定，不是本 lane 能顺带修的。

## BUG-R31-002 策略族共用 createStrategyFact 名称（错误目录插件的条件性风险）

- 状态: 待审核
- 严重级别: 条件性插件误配风险；正确族目录的约定未显示异常
- 模块/文件: `src/WtCore/HftStrategyMgr.cpp`（`loadFactories` L61、L68-71、L76）与
  `src/WtCore/CtaStrategyMgr.cpp`（L61、L68-71、L75）—— 两个管理器用**同一个入口符号名**
  `createStrategyFact` 装载**两种不同类型**的工厂
- 相关测试: `src/TestUnits/coverage/q01_wtcore3/r31_stra_mgr_pluginload.cpp`
  → `r31_stra_mgr.DISABLED_hft_loads_cta_plugin_via_shared_symbol_name`（按门禁约定收进 DISABLED_，不硬闯）
- 输入/前置条件: 把一个「含 CTA 策略插件」的目录误交给 `HftStrategyMgr::loadFactories`。
  本仓库现成就有这份插件（`build-cov/build_x64/Debug/bin/libWtCtaStraFact.dylib` 导出
  `createStrategyFact`，返回 `ICtaStrategyFact*`），而 `src/WtHftStraFact` 不在顶层构建里，
  所以 HFT 侧唯一能凑出该符号的就是 CTA 插件。
- 期望行为: 类型不匹配的插件应当被拒绝并记日志（像 `creator==NULL` 那样 `free_library + continue`），
  最坏也应当可控失败，而不是进程崩溃。
- 源码推导（错误类型插件场景）: `get_symbol(hInst,"createStrategyFact")` 会取得 CTA 的 creator，并被硬转成
  `FuncCreateHftStraFact` 调用，返回的 `ICtaStrategyFact*` 当成 `IHftStrategyFact*` 存进 `_factories`；
  随后将错误接口类型用于 HFT 工厂登记/删除路径，存在未定义行为；具体是否 SIGSEGV 不能仅由源码保证。
  原 lane 运行记录声称：`rc=139 / signal 11`，**3/3 必崩**（只放一份插件也崩，与哈希表扩容无关），
  崩在 gtest `[ RUN ]` 之后、`[ OK ]` 之前，即 `loadFactories` 内部而非用例析构；
  崩溃报告线程栈顶是跳到 `0x0`，上一帧是插件导出的 `deleteStrategyFact`，
  再往下是 `HftStrategyMgr::_StraFactInfo::~_StraFactInfo()` 与 `ankerl::unordered_dense` 插入路径。
  同一批插件喂给 `CtaStrategyMgr`（本文件另外 6 条启用用例）完全正常 ⇒ 差别只在接口类型。此运行记录本复核未独立重跑；对应回归用例为 `DISABLED_`，所以不把 signal 11 或“3/3”当作本次已确认动态结论。共享工厂名是按族目录装载的现存约定，正确 HFT 目录不因此失效；只在错放 CTA 插件时有类型误用风险。若要求错误配置安全拒绝，建议增加族专属符号或类型握手，并在隔离子进程中验证错型插件可控拒绝；正确插件路径应单独通过。
- 复现步骤: `CoverageTests_wtcore --gtest_filter='r31_stra_mgr.DISABLED_hft_loads_cta_plugin_via_shared_symbol_name' --gtest_also_run_disabled_tests`
  （临时目录里只放一份 `libWtCtaStraFact.dylib` 的副本即可）。
- 影响范围（条件性）: 仅在 HFT 插件目录混入 CTA factory 等错型插件时存在启动风险；未证明正确目录或普通生产配置会崩，也未独立核验日志表现。
  覆盖率后果：`HftStrategyMgr.cpp` 78 行里 28 行拿不到（L68-81 成功支 + L94-133 创建口），
  因为这 28 行在现有产物下只能通过类型双关抵达。
- 备注: 根因是「入口符号名不带类型标识」。修复方向三选一：
  (a) 各族插件用各自唯一的入口名（`createHftStrategyFact` / `createSelStrategyFact` 等，和 BUG-R31-001 一起做）；
  (b) 装载后调用一个轻量的类型握手接口（例如工厂上先取 `getFactType()` 再决定进哪张表）；
  (c) 最低成本：`loadFactories` 里对 `getName()` 返回值做可失败处理，并在登记前校验
  `_remover` 也存在，缺任一项就 `free_library + continue` —— 但这只挡得住一部分误配，
  CTA/HFT 同名的本质仍需 (a) 或 (b)。
  本 lane 不改 `src/`，仅登记。

## BUG-R31-003

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtCore/WtExecuterFactory.cpp`（`loadFactories` L61-63）
- 相关测试: `src/TestUnits/coverage/q01_wtcore3/r31_exe_fact_pluginload.cpp`
  （本轮把 L56-65 成功支取满，因此这段「无 NULL 守卫」的码现在处于**被执行**状态；
  其 NULL 分支无法用现有产物覆盖，见 manifest 留白面）
- 输入/前置条件: 任一执行器插件的 `createExecFact()` 返回 NULL
  （第三方插件、构造函数里抛异常后返 NULL、或将来加进来的空实现工厂）。
- 期望行为: 与同族的三个策略管理器一致 —— `CtaStrategyMgr.cpp:68-69`、`SelStrategyMgr.cpp:68-69`、
  `HftStrategyMgr.cpp:68-69` 都写成 `fact = creator(); if (fact != NULL) { 登记 } else { free_library; continue; }`。
- 源码推导: `WtExecuterFactory.cpp:61-63` 是
  `fInfo._fact = fInfo._creator(); _factories[fInfo._fact->getName()] = fInfo;`
  —— 没有 NULL 判定就解引用 ⇒ 空指针读 vtable，崩溃；而且这是同族四份代码里唯一漏判的一份。
- 复核: NULL creator 路径由源码确认，但仓库插件总是返回 `new`，当前没有能触发 NULL 的测试插件，也没有动态崩溃证据。修复方向是在解引用前判空，释放已加载模块并安全跳过/返回失败；定向测试需提供 creator 返回 NULL 的小型 fixture，并断言 loader 不崩且释放一次。另一个同函数观察是同名 factory 重载时 map 覆盖旧结构，若其中工厂对象和 DLL handle 没有 RAII 清理会泄漏；现有 `reloading_same_directory_overwrites_the_entry` 仅验证仍可用，不能证明泄漏实测，应作为独立源码资源所有权审查项，先核对结构析构/容器覆盖语义再决定登记。
- 复现步骤: 需要一份「导出 `createExecFact` 但返回 NULL」的插件才能触发；
  仓库内唯一的执行器插件 `src/WtExeFact/WtExeFact.cpp:32-35` 无条件 `new`，因此本 lane
  没有把它写成启用用例（不允许为覆盖率造产品产物）。代码审查即可确认。
- 影响范围: 目前的产品构建不会命中（本仓库插件恒非空），属外部插件接入时的健壮性缺口；
  同族不一致本身也容易在后续复制粘贴里扩散。
- 备注: 若最终确认 loader 拥有 `hInst` 与 `_fact`，应在失败路径释放模块，并在析构/重复名称登记时按所有权规则清理；当前源码和测试还不足以将重复覆盖泄漏描述为动态实测。
