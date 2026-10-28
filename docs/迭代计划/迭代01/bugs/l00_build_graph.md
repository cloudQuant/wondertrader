# 缺陷记录 — lane L00（主控：构建图）

状态一律「待审核」，本迭代不修改任何生产代码与生产构建脚本。

## BUG-L00-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDtPorter/CMakeLists.txt` L57-L79、`src/WtPorter/CMakeLists.txt`、`src/WtRunner/CMakeLists.txt`、`src/QuoteFactory/CMakeLists.txt`、`src/WtUftRunner/CMakeLists.txt`
- 相关测试: 无（构建期缺陷）。观测入口 `docs/迭代计划/迭代01/tools/coverage.sh` 首轮构建
- 输入/前置条件: 全新（冷）构建目录 + Ninja 并行，例：`cmake -S src -B build-cov -G Ninja -DWT_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-cov -j8`
- 期望行为: 被 POST_BUILD 拷贝的产物（`libParserCTP.dylib` 等）在拷贝命令执行前已由构建图保证存在
- 实际行为: 5 个 CMakeLists 合计 39 条 `add_custom_command(TARGET ... POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy ...)`，而**全仓 `src/**` 的 CMakeLists 中 `add_dependencies` 出现次数为 0**，因此宿主目标与被拷贝的 parser/loader/strategy 目标之间没有排序边。冷目录实测在 `[198/313] Linking ... libWtDtPorter.dylib; depends modules copied` 处失败：
  `Error copying file ".../bin/libParserCTP.dylib" to ".../bin/WtDtPorter/parsers/": No such file or directory`
  同一轮日志中 `ParserCTP.cpp.o` 已在 `[192/313]` 编译完成但尚未链接。
- 复现步骤: 冷目录全量构建（如上）。热目录因上次产物残留会掩盖该问题——这正是本迭代早前「harness 已验证」的结论失效的原因。
- 影响范围: 任何 clean build / CI / 新克隆首次构建都可能随机失败，且失败点随 Ninja 调度而变；`dist/` 下的插件部署目录可能不完整。影响所有平台，非 macOS 独有。
- 备注: 建议修复方向是给这 5 个目标补 `add_dependencies(<host> ParserCTP ParserXTP ... )`，或把部署拷贝从 POST_BUILD 拆成独立的 `add_custom_target(deploy, depends ...)`，让「构建」与「部署」分离。**本迭代未修改任何构建脚本。**
- 被否证的三种绕过（本轮实测，供评审者省时间）：
  1. 「容许构建报错 + 跑两轮」失败：ninja 遇错即停（`ninja: build stopped: subcommand failed`），第二轮把失败的拷贝边排在最前（实测 `[4/101]` 就再次失败），被拷贝的插件仍未链接；且拷贝失败会让整个构建以非 0 结束，测试可执行文件根本没被构建。
  2. `ninja -k 0`（不停止）同样无效：拷贝边失败后，ninja 视宿主 dylib 为未生成，下游依赖（含 `CoverageTests`）会被跳过。
  3. 有效的非侵入绕法是**前置构建被拷贝的插件**：`tools/deploy_prereq_targets.py` 从这 5 个 CMakeLists 里解析出 18 个产物名（`hosts=5 / artifacts=18`），`coverage.sh` 先 `--target` 构建它们再跑全量。`coverage.sh` 用的是这个绕法，属于规避而非修复。

## BUG-L00-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/CMakeLists.txt` L39-L40、`src/WtDataStorageAD/CMakeLists.txt` L39-L40、`src/WtDtPorter/CMakeLists.txt` L51-L52（`CXX_VISIBILITY_PRESET hidden` + `VISIBILITY_INLINES_HIDDEN 1`）
- 相关测试: `src/TestUnits/coverage/l04_datastorage/l04_proc_block_data_test.cpp`、`src/TestUnits/coverage/l05_dt_servo_porter/l05_dt_porter_capi_test.cpp`
- 输入/前置条件: 覆盖率可执行文件 `CoverageTests` 链接 `libWtDataStorage.dylib` / `libWtDtPorter.dylib`，测试调用 `proc_block_data()`、`WtBtDtReader::read_raw_*()`、`ExpDumper::dumpHis*()`
- 期望行为: 测试二进制能通过链接器解析这些已在插件里定义的 C++ 符号
- 实际行为: 链接失败，**15 个 undefined symbol**（`proc_block_data`、`wtp::WtBtDtReader` 的 ctor/dtor/init/5 个 `read_raw_*`、`ExpDumper` 的 5 个 `dumpHis*` 与 `vtable for ExpDumper`）。`nm build-cov/.../libWtDtPorter.dylib | grep ExpDumper` 显示 142 个符号全部为局部符号（类型 `t`），即 hidden visibility 生效。这不是实现缺陷，而是**插件架构使 C++ 内部不可测**——它同时解释了为什么基线里 42 个模块是 0%：除 C API 之外没有任何可链接表面。
- 复现步骤: `bash docs/迭代计划/迭代01/tools/coverage.sh`（在未加入本条绕法前）
- 影响范围: 所有以 hidden 可见性构建的插件模块（WtDataStorage/WtDataStorageAD/WtDtPorter/WtPorter/WtRunner 等）的单元测试可行性；也影响任何想在插件外复用这些类的场景。
- 备注: 建议修复方向是把插件目标改为 `add_library(<mod> OBJECT ...)`，让部署产物与测试共用同一批对象文件；或给测试构一条 `WT_ENABLE_COVERAGE` 下不隐藏可见性的分支。**本迭代未修改生产构建脚本**：当前 `src/TestUnits/coverage/CMakeLists.txt` 的做法是把 `WtDataStorage/*.cpp`、`WtDtPorter/*.cpp` 直接编进 `CoverageTests`——覆盖的生产行相同，但编译单元归属不同，属于规避。dylib 侧这些符号是 local，因此不存在 ODR 冲突。

## BUG-L00-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtCtaStraFact/`、`src/WtCtaTwoMaFact/`、`src/WtUftStraFact/`、`src/WtExeFact/`、`src/WtExecMon/`、`src/WtRiskMonFact/`、`src/WtIndexFactory/`、`src/WtShareHelper/`、`src/WtDtPorter/`、`src/WtDtHelper/`（`WtHelper.cpp` 多份拷贝）
- 相关测试: 覆盖率集成阶段（`cmake --build build-cov --target CoverageTests`）
- 输入/前置条件: 试图把两个及以上插件模块的 `.cpp` 一起编进同一个测试二进制（`BUG-L00-002` 建议的 OBJECT 化方向若按「多插件合一个二进制」实现就会踩到）
- 期望行为: 不同插件模块的源码可以在同一个可执行文件里共存
- 实际行为: **34 个 duplicate symbol**。两类重复：① 插件目录各自复制了 `WtHelper.cpp`（`WtHelper::getCWD()`、`getOutputDir()`、`WtHelper::_inst_dir`），`WtDtPorter` 与静态库里的 `WtHelper` 直接撞名；② 每个策略/执行插件都定义了同名插件入口与同名基类实现（`_createStrategyFact`、`_FACT_NAME`、`_PLATFORM_NAME`、`WtStraFact::createStrategy/deleteStrategy/enumStrategy/getName` 及其 typeinfo），`WtCtaStraFact.cpp` 与 `WtCtaTwoMaFact.cpp` 各贡献 15 处。分插件构建时互不相干，合并即冲突。
- 复现步骤: 把 `PLUGIN_TUNS` 扩到 10 个目录后构建 `CoverageTests`，链接期报 `ld: 34 duplicate symbols`
- 影响范围: 决定「插件类模块到底怎么测」的路线：要么每个插件一个测试二进制，要么测试侧 `dlopen` 已构建的 `.dylib` 再 `dlsym` 入口（覆盖率插桩对 dlopen 的库同样生效）；简单地把多插件源码合进一个测试二进制不可行。
- 备注: 另有一条相关事实：`WtSelStraFact`、`WtHftStraFact` **不在 `src/CMakeLists.txt` 的 `ADD_SUBDIRECTORY` 列表里**（顶层构建不产出这两个目标，实测计数 0），因此它们的源码从未参与 `-Werror` 清洗——把它们编进覆盖率目标会在 `WtHftStraDemo.cpp` 上立刻报 `-Wreorder-ctor` 与 10 处 `-Wunused-parameter`。要么先决定是否纳入构建，要么把它们列入排除清单；「未构建」不等于「可覆盖」。本迭代未修改生产构建脚本，`coverage.sh` 与 `CMakeLists.txt` 的改动都只在自己拥有的测试目标文件里。

## BUG-L00-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtCore/TraderAdapter.h` L567 等 **12 个头文件**（清单见「影响范围」）
- 相关测试: `src/TestUnits/coverage/n01_wtcore_tradepaths/n01_trader_adapter_test.cpp`（该用例在编译期报 `WtCore/TraderAdapter.h:567:41: error: expected class name`）
- 输入/前置条件: 单独包含该头文件：`printf '#include "WtCore/TraderAdapter.h"\nint main(){return 0;}\n' | clang++ -std=c++17 -fsyntax-only -Isrc ...`
- 期望行为: 头文件可独立包含（self-contained），不需要使用者先按特定顺序包含别的头
- 实际行为: 编译失败于 `class TraderAdapterMgr : private boost::noncopyable`——该头使用了 `boost::noncopyable` 却从未 include `boost/noncopyable.hpp`。**双向对照已确认根因**：arm A 只包含 `TraderAdapter.h` → 失败；arm B 先 `#include <boost/noncopyable.hpp>` 再包含它 → 编译通过。
- 复现步骤: 上述单文件 clang++ 自足包含测试（不依赖任何测试二进制）
- 影响范围: 实测总体 = **13 个头文件使用 `boost::noncopyable`，其中 12 个未自行 include**（1 个正确，作为对照）：`WtCore/{TraderAdapter.h, ParserAdapter.h, WtExecMgr.h, WtExecuterFactory.h, CtaStrategyMgr.h, SelStrategyMgr.h, HftStrategyMgr.h}`、`WtDtCore/ParserAdapter.h`、`WtDtServo/ParserAdapter.h`、`WtUftCore/{ParserAdapter.h, UftStrategyMgr.h}`、`Share/threadpool/detail/scope_guard.hpp`。正常构建之所以没暴露，是因为各 `.cpp` 先包含了别的头（Boost.Asio 等）间接带进了声明。后果：这些头无法被单元测试单独包含，测试必须依赖隐式包含顺序。
- 备注: 建议修复方向是在这 12 个头里补 `#include <boost/noncopyable.hpp>`（或在项目内提供一个自带 include 的 `WtNonCopyable` 别名，统一替换）。**本迭代未修改任何生产代码**：lane 已在运行、无法中途下达指令，集成阶段由主控只在**测试文件**里补显式 include 来解除阻塞，产品头的真正修复留待评审。

## BUG-L00-005

- 状态: 待审核
- 严重级别: Blocker（度量完整性）
- 模块/文件: `src/WtCore/`、`src/WtUftCore/`、`src/WtBtCore/`、`src/WtDtCore/` 四处互相重复的 `wtp::` 类定义（`ActionPolicyMgr`、`WtHelper`、`ParserAdapter`、`ParserAdapterMgr`、`TraderAdapter`、`TraderAdapterMgr`、`EventNotifier`、`IParserStub`、`ITrdNotifySink`、`IBaseDataMgr`、`IHotMgr`）
- 相关测试: `n01_wtcore_tradepaths/n01_action_policy_test.cpp` 与 `n03_uftcore_index/n03_action_policy_test.cpp`（同名类的两个 lane）
- 输入/前置条件: 一个测试二进制同时链接 `libWtCore.a` 与 `libWtUftCore.a`（当前 `CoverageTests` 就是如此）
- 期望行为: 每个模块的实现都能被独立链接与独立度量，或链接期就报重复符号
- 实际行为: **两个归档都定义同一个强符号** `T __ZN3wtp15ActionPolicyMgr4initEPKc`（`nm -g -arch arm64` 实测，两边偏移都是 `0x1c0`）。静态库按符号惰性抽取，链接器只从先满足符号的那一份抽取，**不报错、不告警**；另一份实现根本不进二进制，它对应的生产行在该二进制里永远 0 覆盖。于是两个 lane 的 `ActionPolicyMgr` 用例实际跑在同一条实现上，模块级覆盖率归属是假的。
- 复现步骤: `nm -g -arch arm64 build-cov/build_x64/Debug/libs/libWtCore.a | grep ActionPolicyMgr4init`；同命令换 `libWtUftCore.a`。两者都命中即为重复定义。
- 影响范围: 普查到 **37 个跨目录同名 `wtp::` 类**（`re.M` 行锚定，正面控件 `ActionPolicyMgr -> WtCore,WtUftCore`、`ParserAdapter -> WtCore,WtDtCore,WtUftCore`）。重复份数：`WtHelper` 4、`ParserAdapter`/`ParserAdapterMgr`/`EventNotifier` 各 3、`TraderAdapter`/`TraderAdapterMgr`/`ActionPolicyMgr`/`IParserStub`/`ITrdNotifySink` 各 2。这既污染覆盖率归因，也是生产侧的 ODR 隐患：同一份 `wtp::ParserAdapter` 在不同构建配置/链接顺序下可能落到不同实现。
- 备注: 测试侧的对策是**每个核心模块一个测试二进制**，各自只编自己那一族的生产 TU、不再同时链接兄弟核心的 `.a`（`BUG-L00-003` 的 duplicate symbol 就是这个约束的另一面）。产品侧建议：要么把重复类改名（如 `wtp::uft::`），要么把公共部分抽成单一库。**本迭代未修改任何生产代码**；`覆盖排除清单` 需要为「同一二进制内无法归因的模块」新增一条说明。

## BUG-L00-006

- 状态: 待审核
- 严重级别: Blocker（构建一致性 / 度量完整性）
- 模块/文件: `src/CMakeLists.txt:12-14`（`find_package(Boost)` + `include_directories(${Boost_INCLUDE_DIRS})` + `link_directories(${Boost_LIBRARY_DIRS})`）与本机两套 Boost 的共存：`/usr/local/include/boost`＝**1.81**、`/opt/homebrew/include/boost`＝**1.88**
- 相关测试: `CoverageTests_dtcore`（`src/WtDtHelper/WtDtHelper.cpp` 直接编入测试二进制那条路径）
- 输入/前置条件: 同一台机器上 `/usr/local/include/boost` 与 Homebrew boost 同时存在；Apple clang 的内置搜索链把 `/usr/local/include` 排在 `-isystem` 目录之前
- 期望行为: 一个 target 的头文件搜索版本与它实际链接的库版本一致（与生产目标同口径），否则应链接期报错而不是静默 ABI 错位
- 实际行为: 生产目标的编译行是 `-I/opt/homebrew/include` → 目标文件要的是 1.88 形态符号（实测 `nm -u` 生产 `WtDtHelper.cpp.o` 得 `boost::filesystem::detail::path_algorithms::compare_v3`、`directory_iterator_construct(..., directory_options, ...)`），链 Homebrew 1.88 库，一致。测试目标额外多出一处 `-isystem /opt/homebrew/include`（来自被链接目标的 `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES`：Homebrew GTest config 与导出 `${CMAKE_SOURCE_DIR}` 的产品库），于是同一目录以 system 形态登记；clang 按**路径串**去重，此后 `-I` 同一目录直接失效 → 头解析到内置 `/usr/local/include` 的 **1.81**，而 `-lboost_filesystem` 经 `-L/opt/homebrew/lib` 优先解析到 **1.88** 库 → 符号形状不匹配，`CoverageTests_dtcore` 链接失败：`path::compare_v3 / extension_v3 / stem_v3 / detail::directory_iterator_construct(unsigned int 形)` 四个未定义符号，全部 `referenced from ... in WtDtHelper.cpp.o`。
- 复现步骤: 三臂对照，同一份 `#include <boost/version.hpp>`：
  1. `-isystem /opt/homebrew/include` → `BOOST_LIB_VERSION "1_81"`；
  2. `-I /opt/homebrew/include` → `"1_88"`；
  3. `-I` 与 `-isystem` 同时给同一路径（无论先后）→ `"1_81"`，即 `-I` 被去重吞掉。
  真实构建口径取证：`ninja -C build-cov -t commands <CoverageTests_dtcore 的 WtDtHelper.cpp.o>` 与 `<WtDtHelper 生产 .o>` 两条命令的 include 段对比；`nm -u` 两个 `.o` 的 boost 符号形态互为对照。
- 影响范围: 所有 `WT_ENABLE_COVERAGE=ON` 下的测试目标（它们都链接 GTest 导入目标与产品库）。凡是 TU 里用到 `boost::filesystem`/`boost::thread` 的目录都会中招；不用 Boost 的目录只是静默用着 1.81 头，与它链接的 1.88 库存在潜在 ABI 差异（`std::string`/`path` 布局层面本轮未观测到崩溃，属**未验证**而非无害）。
- 备注: 测试侧的规避已经落地且**只作用于测试目标**：在 `src/TestUnits/coverage/CMakeLists.txt` 里建 `<builddir>/cov_boost_include/boost -> ${Boost_INCLUDE_DIR}/boost` 符号链接并把该目录放 `-I` 链首（clang 只对同一路径串去重，换名即绕开），实测 `CoverageTests_dtcore` 由 exit=1 变 exit=0。产品侧建议：`find_package(Boost)` 后显式约束 `Boost_INCLUDE_DIR` 与 `Boost_LIBRARY_DIR` 同源（或改用 `Boost::headers` 导入目标，让头/库版本由同一 target 决定），并检查 `/usr/local` 遗留安装是否仍在开发机上与 Homebrew 争抢。另一种更彻底的方案（把 `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` 清零）本轮**未采用**，因为那是产品库的接口属性，会同时改变 WtRunner/QuoteFactory 等可执行目标的告警面，超出「只动测试」的边界。


## BUG-L00-007

- 状态: 待审核
- 严重级别: Blocker（构建完整性）
- 模块/文件: `src/TestUnits/coverage/CMakeLists.txt` 的 `WT_MAIN_MODULES` 循环（m_<Module> 目标），涉及 `src/WtUftRunner/CMakeLists.txt:21-22`、`src/WtLatencyUFT/CMakeLists.txt:21-22`
- 相关测试: `m_WtUftRunner/r14_wtuftrunner_main_test.cpp`、`m_WtLatencyUFT/r14_uftlatency_main_test.cpp`
- 输入/前置条件: 为带 `main()` 的可执行体建覆盖率测试目标（编译期 `main=wt_coverage_main_<mod>` 重命名），测试目标只链 `WT_COV_SHARED_LIBS`
- 期望行为: 测试目标链接的「核心静态库」应与产品可执行体自己的一致
- 实际行为: `CoverageTests_m_WtUftRunner` 与 `CoverageTests_m_WtLatencyUFT` 链接失败：`ShareManager::initialize/init_domain/start_watching` 等未定义 —— 产品的 `WtUftRunner`/`WtLatencyUFT` 都是 `SET(LIBS WtUftCore …)`，而 `WtUftCore` 是 **STATIC**，既不在 dylib 集合里、也不在 `WT_COV_SHARED_LIBS` 里，没有任何出处。`m_WtRunner`/`m_QuoteFactory`/`m_WtLatencyHFT` 侥幸能链，是因为它们要的核心符号恰好能从已链的 `libWtPorter.dylib` 等导出里取到 —— 也就是说「能不能链上」取决于导出巧合，不是设计。
- 复现步骤: `cmake --build build-cov --target CoverageTests_m_WtUftRunner`（修复前 exit=1，报上面那组未定义符号）
- 影响范围: 全部 7 个 `m_<Module>` 目标；也说明「链接成功」在补齐前不能作为「该可执行体已被正确插桩」的证据。
- 备注: 测试侧已修：循环里从**该模块自己的 CMakeLists** 扫 `WtCore|WtBtCore|WtUftCore|WtDtCore` 出现与否，把命中的加进 `TARGET_LINK_LIBRARIES`（不手敲清单）。实测 configure 输出 `m_WtUftRunner 附加核心静态库 [WtUftCore]`、`m_WtRunner [WtCore]`、`m_QuoteFactory [WtDtCore]`、`m_LoaderRunner []`，五个目标全部 exit=0（原先两个失败、三个成功）。产品侧建议：给这些可执行体引入与库分离的 OBJECT 层，使「谁提供了哪些符号」由构建图显式表达，而不是靠 dylib 导出兜住。注意与 BUG-L00-005 的约束：一个测试二进制只准链一个核心，跨核心的同名 `wtp::` 类会静默择一。

## BUG-L00-008

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/Share/ModuleHelper.hpp:48`（另见 `src/TraderDD/TraderDD.cpp:54`、`src/TraderHTS/TraderHTS.cpp:56` 各自又写了一份同名定义）
- 相关测试: `src/TestUnits/coverage/r16_share_tools/r16_logger_dll_test.cpp`、主目标 `CoverageTests`
- 输入/前置条件: 任意两个包含 `ModuleHelper.hpp` 的 TU 被链进**同一个**可执行体/静态库。
  本轮触发方式：覆盖率主目标把产品 TU `WtDtPorter/WtDtPorter.cpp` 与用例 TU
  `r16_logger_dll_test.cpp` 一起链进 `CoverageTests`（这是主目标采集产品行覆盖率的必要做法）。
- 期望行为: 头文件里的辅助空函数应写成 `inline` 或放进匿名/`static` 存储期，使多 TU 包含合法。
- 实际行为: `void inst_hlp() {}` 是**外部链接、非 inline** 的定义，每个包含它的 TU 都输出一份
  `T __Z8inst_hlpv`（`nm` 实测三个 .o 各有一份：`WtDtPorter.cpp.o`、`WtDtPorter/WtDtRunner.cpp.o` 同族、
  `r16_logger_dll_test.cpp.o`）→ 链接期 `duplicate symbol 'inst_hlp()'`，`ld: 1 duplicate symbols`，
  整个 `CoverageTests` 目标链不通（`clang++: error: linker command failed with exit code 1`）。
- 复现步骤: `cmake --build build-cov --target CoverageTests`（在未加改名隔离的树上，可 `git stash` 本次
  `src/TestUnits/coverage/CMakeLists.txt` 的 `WT_COV_ISOLATE_INST_HLP` 后重跑对照）。
- 影响范围: 任何把两个 `ModuleHelper.hpp` 包含者放进同一链接单元的场景都会失败 —— 除测试外也包括
  「把 `TraderDD.cpp`/`TraderHTS.cpp` 与任何引到该头的 TU 同链」的情形（那两个文件自己又重定义了同名符号，
  与头里的定义构成三份）。产品当前各自独立成 dylib，所以线上没有暴露。
- 备注: 本轮**不改产品代码**。已在测试侧用按源文件路径哈希的唯一改名绕过
  （`CMakeLists.txt` 里的 `WT_COV_ISOLATE_INST_HLP`，只对真的包含该头的 TU 生效；
  `getInstPath()` 本身是 `static`、每 TU 各取自身地址，行为不变）。
  修复方向（待评审）：头里改成 `inline void inst_hlp() {}` 或匿名命名空间 + `static`，
  并删掉 `TraderDD.cpp`/`TraderHTS.cpp` 里的重复定义。

## BUG-L00-009

- 状态: 待审核
- 严重级别: Minor（运行时危害今天为零，但它是「这 4 个模块进不了分母」的直接原因）
- 模块/文件: `src/ParserYD/ParserYD.cpp:54-56` 对照 `src/ParserYD/ParserYD.h:69-70`
- 相关测试: 无（该模块在本机从未被编译，见 `覆盖排除清单.md` 第 6 节）
- 输入/前置条件: 用第一方构建的告警门槛编译该 TU：
  `-Wall -Wextra -Werror -std=gnu++17`（厂商头存在，只差 `-I src/API/yd1.108.360`）。
- 期望行为: 构造函数的成员初始化列表顺序与类内声明顺序一致（C++ 实际按**声明顺序**初始化，列表顺序只是表象）。
- 实际行为: 声明是 `m_uTradingDate`(.h:69) 在前、`m_pUserAPI`(.h:70) 在后，
  初始化列表写成 `m_pUserAPI(NULL), m_uTradingDate(0)` ⇒ 编译器报
  `error: field 'm_pUserAPI' will be initialized after field 'm_uTradingDate' [-Werror,-Wreorder-ctor]`。
  两处初值都是常量（NULL / 0），**所以当前不存在可观察的错误行为**；风险在于将来有人把初值换成
  依赖另一个成员的表达式时，实际初始化顺序与代码表面顺序不一致会静默出错。
  同一 TU 还有 6 处 `-Werror,-Wunused-parameter`（`:68/:80/:276` 等，厂商回调签名里未用的形参）。
- 复现步骤: `cmake --build build-cov --target CoverageTests_t_ParserYD`（需先存在已登记的
  `src/TestUnits/coverage/t_ParserYD/` 用例目录；主控验证时临时建过、已删除）。
- 影响范围: `ParserYD` / `ParserHuaX` / `TraderATP` / `TraderXTPXAlgo` 四个厂商适配层
  （合计约 4,117 行源码）都卡在同类告警上，因此**不在覆盖率分母里**；
  按现有收发族实测水平估计可再拿下约 6,186 行（含 `ParserOES`）。
- 备注: 本条只登记，不改 `src/**`。owner 需在两条路里选：
  （1）产品侧修这些告警 + 给 `TraderHuaX.cpp:804` 的 `strcpy_s` 加非 MSVC 兼容层；
  （2）授权测试侧对这些生产 TU 单独 `-Wno-error=unused-parameter,reorder-ctor`。
  主控不自行选（那等于替评审决定「这些告警可以不修」）。
