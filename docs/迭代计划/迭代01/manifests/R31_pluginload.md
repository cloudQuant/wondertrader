# R31_pluginload 用例清单（lane R31，WtCore 第八波：插件装载「成功支路」）

构建归属：`CoverageTests_wtcore`。用例写进**已登记目录** `src/TestUnits/coverage/q01_wtcore3/`
（分派表 `src/TestUnits/coverage/CMakeLists.txt:92` 的 `dir:q01_wtcore3`），文件名前缀 `r31_`。
**没有新建用例目录、没有改任何 CMakeLists、没有改 `src/` 产品代码、没有 git add/commit/push。**

| 文件 | 用例数 | 启用 | DISABLED | 门禁（`--only '^r31'`） |
|---|---|---|---|---|
| `r31_exe_fact_pluginload.cpp` | 10 | 10 | 0 | exit 0，10/10 PASS |
| `r31_stra_mgr_pluginload.cpp` | 11 | 10 | 1 | exit 0，10/10 PASS |
| 合计 | 21 | **20** | 1 | 20 条全绿（0 FAIL / 0 CRASH / 0 SUITECONF / 0 未记账） |

fixture 各自收在本文件的匿名命名空间里，且**每个 .cpp 只有一套 fixture**（`r31_exe_fact` 与 `r31_stra_mgr`
是两个不同的 suite 名），没有任何 fixture 类进头文件 —— 规避了「同名头被 2 个以上 TU 包含 ⇒ 同名不同种
⇒ gtest 运行期拒绝整族」这一条（本轮 SUITECONF = 0）。
两个文件都**不**包含 `q01_sel_env.h`（本 lane 只用真实插件，不用 Sel 替身）。

## 门禁记录（一律用仓库脚本）

| 命令 | 退出码 | 结果 |
|---|---|---|
| `lane_gate.sh CoverageTests_wtcore --build --jobs 3 --only '^r31'` | **0** | rows=20 enumerated=20 unaccounted=0，非通过合计 = 0 |
| `lane_gate.sh CoverageTests_wtcore --jobs 3`（收尾整目标） | **5** | 348 条：291 PASS / 46 FAIL / 11 CRASH；**r31 的 20 条全在 PASS 里**，57 条红全部属于兄弟 lane（清单见文末） |

## 本 lane 的存在理由：成功支路这次是不是真的走到了 creator 非空 → 实例化

**走到了，而且是可证伪地走到。** 三条独立证据：

1. **断言只能由成功支路满足。** `WtExecuterFactory::_factories` 与 `CtaStrategyMgr::_factories`
   都是 private、没有任何进程内注册口，唯一的填法就是 `loadFactories` 扫目录。
   用例把真实 `build-cov/build_x64/Debug/bin/libWtExeFact.dylib`（绝对路径由 `dladdr` 推出）
   复制进临时目录后调用 `loadFactories`，随后
   `createExeUnit("WtExeFact","WtTWapExeUnit")` 返回**非空**、`unit->self()->getFactName()=="WtExeFact"`
   且 `getName()=="WtTWapExeUnit"` —— 这只有在 L49 `get_symbol("createExecFact")` 非空、
   L61 `_creator()` 真的 `new WtExeFact`、L63 真的按 `getName()` 登记之后才可能成立。
   CTA 侧同理：`createStrategy("WtCtaStraFact","DualThrust",…)` 与
   `createStrategy("WtCtaTwoMaFact","TwoMa",…)` **同时**返回非空。
2. **行覆盖 A/B（同一个二进制，只切换是否跑 r31 用例，`llvm-cov report` 单对象口径）**：

   | 生产文件 | 总行 | 本轮 before missed | after missed | 差 |
   |---|---|---|---|---|
   | `src/WtCore/WtExecuterFactory.cpp` | 130 | 63 | **2** | −61 |
   | `src/WtCore/CtaStrategyMgr.cpp` | 78 | 28 | **5** | −23 |
   | `src/WtCore/SelStrategyMgr.cpp` | 78 | 41 | **27** | −14 |
   | `src/WtCore/HftStrategyMgr.cpp` | 78 | 28 | 28 | 0（见留白面） |

   before = 328 条非 r31 用例的 profraw 合并；after = 328 + 本 lane 20 条合并。
   主控在 `接缝阻塞面-20261027.md` §5 记的 `WtExecuterFactory.cpp 130 行 missed=69`
   是上一轮 raw 行口径（且二进制比本轮旧），本轮同口径量到 63 —— 两者都指向同一段
   L49-65 成功块，不影响结论。
3. **符号复用被证伪（不是假设）。** `libWtCtaStraFact.dylib` 与 `libWtCtaTwoMaFact.dylib`
   导出**同名**的 `createStrategyFact`，两份又同时静态挂在 `CoverageTests_wtcore` 上
   （`otool -L` 实证）。用例 `cta_two_plugins_in_one_dir_register_under_their_own_names`
   把两份一起放进一个目录：若 `dlsym(handle,…)` 发生跨镜像复用，两个工厂名只会剩下一个，
   断言立刻红。实测两个都在 ⇒ macOS 两级命名空间下 `dlopen(各自绝对路径) + dlsym(该 handle)`
   拿到的是各自镜像的实现。

**对照产物侧事实**（`nm -gU`，用例里也有等价的可执行探针）：
`libWtExeFact.dylib` 导出 `_createExecFact/_deleteExecFact`；`libWtCtaStraFact.dylib`、
`libWtCtaTwoMaFact.dylib` 导出 `_createStrategyFact/_deleteStrategyFact`；
`libWtIndexFactory.dylib` 只导出 `_create_index_factory/_delete_index_factory`。

## 用例 → 生产文件:行段（行号取自本 lane 实读的源码）

### r31_exe_fact_pluginload.cpp → src/WtCore/WtExecuterFactory.cpp（130 行）

| 用例 | 行段 | 证什么 |
|---|---|---|
| product_dylib_on_disk_exports_the_entry_symbols | 不经生产码（`DLLHelper` 探针） | 产物存在 + `createExecFact/deleteExecFact` 可解析，且 `libWtIndexFactory` 确实不含 `createExecFact`（后面混合目录用例的前提） |
| loadfactories_with_real_dylib_reaches_creator_and_registers | **L43-47 dlopen 成功**、**L49-54 creator 非空**、**L56-61 组包 + `_creator()` 实例化**、**L63 登记**、**L65 info 日志**、L71-84 `createExeUnit(fact,unit)` 成功支 | 成功支路本体 |
| registered_factory_creates_every_plain_unit_name | L73-84（5 个普通单元名 × 成功支 + `NoSuchUnit` ⇒ **L79-82 error 日志**） | 登记后 `unit==NULL` 与成功两支都真 |
| registered_factory_creates_diff_unit_and_never_arbi | L89-100（diff 成功 + 未知名 ⇒ L95-98）、**L109-115**（arbi 恒 NULL ⇒ 错误日志支） | 差价口成功、套利口「已登记但造不出」 |
| dotted_name_overloads_hit_the_registered_factory | L121-139（split→查表命中→成功）、**L144-162**（diff 点号名成功 + 未知 ⇒ L156-160）、L167-185（arbi 点号名 ⇒ L180-183 error）、三个重载的 `ay.size()<2` 早退（L122/L145/L168） | 单参重载的成功支（n02 只测过「未注册」形态） |
| unknown_factory_name_still_misses_after_success | L73-75、L89-91、L105-107、L128-130、L151-153、L174-176（`find==end`） | 登记的键真的是 `IExecuterFact::getName()` 而不是文件名 |
| mixed_directory_scans_success_and_skips_alongside | L27-28（目录 continue，含目录名带 `.dylib`）、L34-35（非 dylib continue）、L44-46（非 Mach-O dlopen 失败 continue）、L50-53（`libANoFact.dylib` 有镜像无符号 ⇒ **free_library + continue**）、L56-65（同一次扫描里的成功项） | 三支 continue 与成功支交错；补 R15 记过但本轮实测为 0 的 L52 |
| reloading_same_directory_overwrites_the_entry | 第二次进入 L43-65（同路径 dlopen 引用计数 + L63 同名键覆盖） | 重复 `loadFactories` 不产生错乱 |
| wrapper_from_loaded_factory_releases_through_plugin | L84（`new ExeUnitWrapper(unit, fInfo._fact)`）、`WtExecuterFactory.h` L29/L35-41/L47（ctor/dtor 经**插件工厂**回收、shared_ptr 双引用只释放一次） | 装载成功之后才有真工厂可回收（n02 用的是替身工厂） |
| arbi_unit_ptr_stays_empty_and_wrapper_not_built | L110-115 | 空 `ExecuteUnitPtr` 不解引用 |

### r31_stra_mgr_pluginload.cpp → src/WtCore/CtaStrategyMgr.cpp（78 行）

| 用例 | 行段 | 证什么 |
|---|---|---|
| cta_plugins_export_create_strategy_fact_entry | 不经生产码 | CTA 两份插件有 `createStrategyFact/deleteStrategyFact`，执行器与指标插件没有 |
| cta_load_reaches_creator_and_registers_real_factory | **L57-59 dlopen 成功**、**L61-66 creator 非空**、**L68 `creator()` 实例化**、**L69-81 登记 + info + count++**、**L90/L92**、L97-104（`createStrategy(fact,unit,id)` 成功 + `_strategies` 登记） | CTA 成功支路本体 |
| cta_two_plugins_in_one_dir_register_under_their_own_names | L57-81 两次进入，键分别是 `WtCtaStraFact` / `WtCtaTwoMaFact` | 跨镜像同名符号**没有**被复用（本 lane 最重要的一条反证） |
| cta_dotted_name_overload_creates_and_registers | **L109-123**（split→命中→成功）、L126-133（`getStrategy` **命中支** L132）、L110-111（三段名取前两段 / 不足两段早退） | 点号形态的成功支 + 策略表命中 |
| cta_registered_factory_with_unknown_unit_wraps_null | L101-104（工厂已登记、单元不认识 ⇒ 包装 NULL）+ L128-132 | `CtaStraWrapper` 携 NULL 时析构不调 `deleteStrategy`（作用域退出无崩溃） |
| cta_mixed_dir_interleaves_success_with_three_continue_branches | L43-45、L50-51、L58-59、**L62-66 free_library 支**、L57-81 成功项 | 目录 continue、非 dylib continue、dlopen 失败 continue、缺符号 free_library 与成功支同批走 |
| cta_reload_same_dir_overwrites_the_factory_entry | 第二次进入 L57-81，L71 `_factories[getName()]` 命中已有项并覆盖 | 重复装载不产生错乱；同一 id 二次 create 返回同一个包装 |

### r31_stra_mgr_pluginload.cpp → src/WtCore/SelStrategyMgr.cpp（78 行）

| 用例 | 行段 | 证什么 |
|---|---|---|
| no_product_plugin_exports_the_sel_entry_symbol | 不经生产码（12 份产物逐个 `dlopen+dlsym` 探针） | `createMfStrategyFact` 在本机**每一份**产品插件里都不存在 ⇒ BUG-R31-001 的事实基础 |
| sel_load_of_plugin_like_dir_always_misses_its_entry_symbol | L57-59（4 份真插件逐个 dlopen 成功）、**L61-66（creator==NULL ⇒ free_library + continue）**、L88-92（count=0 日志 + return true）、L97-99 / L112-118 / L126-131（空表四个查询口） | SEL 装载循环第一次真的跑起来（n01/r15 都没喂过 Sel）；同时证明 SEL 插件恒装载不进来 |
| sel_mixed_dir_with_unloadable_and_text_files_still_returns_true | L43-45、L50-51、L58-59、L61-66 | 目录 continue、扩展名 continue、非 Mach-O continue 与缺符号 free_library |

## 留白面（带原因与行数量级）

| 面 | after 未覆盖行 | 原因 |
|---|---|---|
| `WtExecuterFactory.cpp` **L116、L185**（`createArbiExeUnit` 两个重载的成功 `new ExeUnitWrapper`） | 2 行 | 本仓库唯一的执行器插件 `WtExeFact::createArbiExeUnit`（`src/WtExeFact/WtExeFact.cpp:126-129`）**恒返 NULL**，注释写明「当前暂不支持任何套利执行单元」。没有会产套利单元的产品插件，这两行拿不到；不是用例写法问题，也不是缺陷（文档行为）。 |
| `CtaStrategyMgr.cpp` **L82-86**（`creator()` 返回 NULL ⇒ `free_library + continue`） | 5 行 | `WtCtaStraFact/WtCtaTwoMaFact` 的 `createStrategyFact()` 无条件 `new`，永不返回 NULL ⇒ 需要「导出 createStrategyFact 但返回 NULL」的插件才行；不允许为覆盖率造产品产物。 |
| `SelStrategyMgr.cpp` **L68-88 + L101-104 + L120-123 + L132** | 27 行 | 入口符号名 `createMfStrategyFact`（L61）在全仓库无出处，且 `src/WtSelStraFact` 根本不在顶层 `ADD_SUBDIRECTORY` 里 ⇒ SEL 成功支与「有工厂之后的三个创建口」恒不可达。这是产品缺陷，已登 BUG-R31-001；要拿这 27 行得先修符号名并把 Sel 插件纳入构建。 |
| `HftStrategyMgr.cpp` **L68-81（成功支）+ L94-133（创建口）** | 28 行 | 唯一能凑出 `createStrategyFact` 的产物是 CTA 插件；把它当 `IHftStrategyFact` 用属类型双关，实测在 `loadFactories` 的登记路径里 SIGSEGV（3/3，见 DISABLED 用例注释与 BUG-R31-002）。主控提示「出现符号复用就收进 DISABLED_，不要硬闯」⇒ 本 lane 不取这 28 行。真正的 HFT 插件 `src/WtHftStraFact` 同样不在顶层构建里。 |
| 跨核心复用提示 | — | 本 lane 打通的「真 dylib + 绝对路径 + 临时目录」这套做法，可以直接解掉 R10 manifest L116 与 R15 留白表里「`WtLocalExecuter::getUnit` 建单元成功后的循环体 ~30 行、BUG-R10-002 工厂缝未解」这一类判定：`WtLocalExecuter`/`WtExecMgr` 只要拿到本 lane 的 `WtExecuterFactory` 就能有真工厂。属于后续 lane 的面积，本 lane 不扩范围。 |

## 更正记录（避免后人再当接缝）

- `manifests/R10_wtcore.md:116`「`WtExecuterFactory::_factories` 只能扫目录加载 dylib，无进程内注册口」⇒ 结论没错，但据此把 210 行记成不可回收是错的：**目录里放真 dylib 就是注册口**，本轮该文件 130 行只剩 2 行未覆盖。
- `manifests/n02_wtcore_engines.md:69`「成功支需要真实可 dlopen 的执行器插件 .dylib，本 lane 禁止触发构建」⇒ 产物早已存在（`build-cov/build_x64/Debug/bin/`），无需触发构建，本轮补齐。
- `接缝阻塞面-20261027.md` §5 的判定成立，本 lane 是它的落实与量化。

## 兄弟 lane 的红（本轮整目标 exit 5 的构成，未代改）

348 条里 57 条非通过（46 FAIL + 6 SIGSEGV + 4 SIGBUS + 1 SIGABRT），按 suite：
R3CtaTest 16、R3TraderTest 6、R10SelEngineTest 5、N02ExecMgrTest 5、R15SelTest 4、
P01CtaQueryTest 3、R10HftTest 2、P11SyncReport 2、P11PreOrderRisk 2、P02EngineTest 2、
N01ParserAdapter 2、R15CtxTest 1、R10EvtTest 1、Q01HftCtxTest 1、P11PushPersist 1、
P11PushNotify 1、P02DiffExecuterTest 1、N02ExecuterFactoryTest 1、N01ActionPolicy 1。

这 57 条**没有一条属于 r31**（红名单里 `^r31` 命中数 = 0），而且整目标红数在三次复跑里是
56 / 57 / 58 浮动：浮动的成员都落在 `P02EngineTest`/`P02DiffExecuterTest` 一族
（例：`P02EngineTest.LoadDatasParsesFundPositionsAndRiskMon` 在整目标跑里红、
在「只跑 328 条非 r31 用例」的独立跑里绿；`P02DiffExecuterTest.SaveDataCreatesFileWhenNothingLoaded`
则相反）⇒ 并发/临时目录相关的既有抖动，两个方向都会翻，不是本轮引入。

**归因核对（不是「大概不是我的」，是逐名比过）**：把 328 条非 r31 用例单独跑一遍取红得到 58 条，
与整目标那 57 条逐名 join 后，**两个集合都不包含任何 r31 用例**，交集差集全部落在上述 P02 抖动族
⇒ 本 lane 的 20 条没有给任何兄弟 suite 添红；上述红是承接的既有状态，
按门禁约定留给各自 lane（R3/R10/R15/N01/N02/P01/P02/P11/Q01），本 lane 未代改。
