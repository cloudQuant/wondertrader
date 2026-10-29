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
- 影响范围: 使用并行调度、且宿主目标没有其它隐含目标依赖的 clean build 可能在 POST_BUILD 拷贝时遇到尚未生成的产物；具体影响平台和生成器范围仍待验证。当前证据来自 macOS/Ninja，不据此宣称所有平台均会失败。
- 备注: 建议修复方向是给这 5 个目标补 `add_dependencies(<host> ParserCTP ParserXTP ... )`，或把部署拷贝从 POST_BUILD 拆成独立的 `add_custom_target(deploy, depends ...)`，让「构建」与「部署」分离。**本迭代未修改任何构建脚本。**
- 被否证的三种绕过（本轮实测，供评审者省时间）：
  > 以下是历史实测记录；2026-10-29 复核未重跑这些构建命令。
  1. 「容许构建报错 + 跑两轮」失败：ninja 遇错即停（`ninja: build stopped: subcommand failed`），第二轮把失败的拷贝边排在最前（实测 `[4/101]` 就再次失败），被拷贝的插件仍未链接；且拷贝失败会让整个构建以非 0 结束，测试可执行文件根本没被构建。
  2. `ninja -k 0`（不停止）同样无效：拷贝边失败后，ninja 视宿主 dylib 为未生成，下游依赖（含 `CoverageTests`）会被跳过。
  3. 有效的非侵入绕法是**前置构建被拷贝的插件**：`tools/deploy_prereq_targets.py` 从这 5 个 CMakeLists 里解析出 18 个产物名（`hosts=5 / artifacts=18`），`coverage.sh` 先 `--target` 构建它们再跑全量。`coverage.sh` 用的是这个绕法，属于规避而非修复。

- 复核结论（2026-10-29）: **构建图缺边已由源码确认；冷构建失败日志和修复后冷构建结果未在本次复核中验证。** 当前五个宿主仍有 POST_BUILD 拷贝，`src/**/CMakeLists.txt` 未找到 `add_dependencies`；现存 `build-cov` 是已有构建目录，不能作为冷构建证据。`coverage.sh` 有前置构建绕法，但其 `cmake --build ... --target $PRE_TARGETS || true` 会吞掉该前置步骤错误，后续全量构建仍需成功才可判通过。建议在临时全新目录分别记录 Ninja 冷构建和目标预构建绕法的完整命令、退出码及日志；生产侧可按每个宿主真实拷贝清单显式建目标依赖，或把部署拷贝建成有依赖边的独立目标。脚本的 `python3` 调用也需遵守本仓环境要求，改用 Anaconda base。

## BUG-L00-002

- 状态: 已撤回（内部 C++ 符号未导出是测试链接边界，未证实违反产品 API）
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/CMakeLists.txt` L39-L40、`src/WtDataStorageAD/CMakeLists.txt` L39-L40、`src/WtDtPorter/CMakeLists.txt` L51-L52（`CXX_VISIBILITY_PRESET hidden` + `VISIBILITY_INLINES_HIDDEN 1`）
- 相关测试: `src/TestUnits/coverage/l04_datastorage/l04_proc_block_data_test.cpp`、`src/TestUnits/coverage/l05_dt_servo_porter/l05_dt_porter_capi_test.cpp`
- 输入/前置条件: 覆盖率可执行文件 `CoverageTests` 链接 `libWtDataStorage.dylib` / `libWtDtPorter.dylib`，测试调用 `proc_block_data()`、`WtBtDtReader::read_raw_*()`、`ExpDumper::dumpHis*()`
- 期望行为: 测试二进制能通过链接器解析这些已在插件里定义的 C++ 符号
- 实际行为: 历史覆盖率链接尝试记录为 **15 个 undefined symbol**（`proc_block_data`、`wtp::WtBtDtReader` 的 ctor/dtor/init/5 个 `read_raw_*`、`ExpDumper` 的 5 个 `dumpHis*` 与 `vtable for ExpDumper`）；当时 `nm ...libWtDtPorter.dylib` 记录的 `ExpDumper` 符号为局部符号，符合 hidden visibility。此现象说明测试二进制不能直接链接这些未导出的 C++ 符号；是否构成产品缺陷取决于这些类是否承诺为插件外部 API。hidden 可见性本身不能解释 42 个模块均为 0%：未执行、未纳入测试目标或未装载插件也会产生零覆盖，需逐模块查覆盖对象与报告。
- 复现步骤: `bash docs/迭代计划/迭代01/tools/coverage.sh`（在未加入本条绕法前）
- 影响范围: 所有以 hidden 可见性构建的插件模块（WtDataStorage/WtDataStorageAD/WtDtPorter/WtPorter/WtRunner 等）的单元测试可行性；也影响任何想在插件外复用这些类的场景。
- 备注: 建议修复方向是把插件目标改为 `add_library(<mod> OBJECT ...)`，让部署产物与测试共用同一批对象文件；或给测试构一条 `WT_ENABLE_COVERAGE` 下不隐藏可见性的分支。**本迭代未修改生产构建脚本**：当前 `src/TestUnits/coverage/CMakeLists.txt` 的做法是把 `WtDataStorage/*.cpp`、`WtDtPorter/*.cpp` 直接编进 `CoverageTests`——覆盖的生产行相同，但编译单元归属不同，属于规避。dylib 侧这些符号是 local，因此不存在 ODR 冲突。

- 复核结论（2026-10-29）: **测试链接边界/插件架构约束已确认；尚无证据表明它是违反产品 API 的缺陷。** 当前测试 CMake 确实把相关源码 TU 编入测试目标，且当前构建目录已有 dtcore 等目标产物；本次未重跑旧的 15 符号失败用例。建议把建议修复方向限定为“如果需要从插件外调用这些 C++ 类”，并优先评估独立测试目标或通过插件导出 C 接口测试；不要将这一限制直接作为 42 个零覆盖模块的共同根因。若要保留 42 的判断，需附逐模块覆盖报告及其目标/装载状态。

## BUG-L00-003

- 状态: 已撤回（多插件合并链接的测试架构限制；独立插件不因该项失效）
- 严重级别: Major
- 模块/文件: `src/WtCtaStraFact/`、`src/WtCtaTwoMaFact/`、`src/WtUftStraFact/`、`src/WtExeFact/`、`src/WtExecMon/`、`src/WtRiskMonFact/`、`src/WtIndexFactory/`、`src/WtShareHelper/`、`src/WtDtPorter/`、`src/WtDtHelper/`（`WtHelper.cpp` 多份拷贝）
- 相关测试: 覆盖率集成阶段（`cmake --build build-cov --target CoverageTests`）
- 输入/前置条件: 试图把两个及以上插件模块的 `.cpp` 一起编进同一个测试二进制（`BUG-L00-002` 建议的 OBJECT 化方向若按「多插件合一个二进制」实现就会踩到）
- 期望行为: 不同插件模块的源码可以在同一个可执行文件里共存
- 实际行为: **34 个 duplicate symbol**。两类重复：① 插件目录各自复制了 `WtHelper.cpp`（`WtHelper::getCWD()`、`getOutputDir()`、`WtHelper::_inst_dir`），`WtDtPorter` 与静态库里的 `WtHelper` 直接撞名；② 每个策略/执行插件都定义了同名插件入口与同名基类实现（`_createStrategyFact`、`_FACT_NAME`、`_PLATFORM_NAME`、`WtStraFact::createStrategy/deleteStrategy/enumStrategy/getName` 及其 typeinfo），`WtCtaStraFact.cpp` 与 `WtCtaTwoMaFact.cpp` 各贡献 15 处。分插件构建时互不相干，合并即冲突。
- 复现步骤: 把 `PLUGIN_TUNS` 扩到 10 个目录后构建 `CoverageTests`，链接期报 `ld: 34 duplicate symbols`
- 影响范围: 决定「插件类模块到底怎么测」的路线：要么每个插件一个测试二进制，要么测试侧 `dlopen` 已构建的 `.dylib` 再 `dlsym` 入口（覆盖率插桩对 dlopen 的库同样生效）；简单地把多插件源码合进一个测试二进制不可行。
- 备注: 另有一条相关事实：`WtSelStraFact`、`WtHftStraFact` **不在 `src/CMakeLists.txt` 的 `ADD_SUBDIRECTORY` 列表里**（顶层构建不产出这两个目标，实测计数 0），因此它们的源码从未参与 `-Werror` 清洗——把它们编进覆盖率目标会在 `WtHftStraDemo.cpp` 上立刻报 `-Wreorder-ctor` 与 10 处 `-Wunused-parameter`。要么先决定是否纳入构建，要么把它们列入排除清单；「未构建」不等于「可覆盖」。本迭代未修改生产构建脚本，`coverage.sh` 与 `CMakeLists.txt` 的改动都只在自己拥有的测试目标文件里。

- 复核结论（2026-10-29）: **合并插件源码的重复定义风险合理；“34”及原复现步骤属于历史记录，本次未复现。** 当前 `src/TestUnits/coverage/CMakeLists.txt` 不含文中 `PLUGIN_TUNS` 变量，而是使用分目标、`CTA_FACTORY_TUS` 和对部分符号的编译期别名；因此文中步骤不能直接在当前树复跑。“每插件一个二进制或 dlopen”也不是穷尽方案，当前测试已通过源码组合和符号别名建立其它测试目标。建议把精确的 34 符号数标为历史实测并附原始链接日志；产品影响应限定在多个插件 TU 确实合入同一最终链接单元时。`WtSelStraFact`/`WtHftStraFact` 当前仍不在产品顶层列表，但覆盖 CMake 会编译它们且对这些 TU 放宽告警；应分别描述产品门槛与覆盖目标门槛。

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

- 复核结论（2026-10-29）: **头文件缺少直接声明依赖已由源码确认；本次未重跑 standalone 编译。** `TraderAdapter.h` 顶部没有 noncopyable include，目标类在后文继承 `boost::noncopyable`；当前对应测试显式包含 `<boost/core/noncopyable.hpp>`，所以现有测试通过不能证明产品头自足。建议按仓库支持的 Boost 版本给所有受影响头文件直接包含提供 `boost::noncopyable` 的头（本机测试使用的是 `boost/core/noncopyable.hpp`），再用单头语法编译复核；这属于头文件可编译性缺陷，严重级别应按公开头文件的使用范围审核。

## BUG-L00-005

- 状态: 待审核
- 严重级别: Blocker（度量完整性）
- 模块/文件: `src/WtCore/`、`src/WtUftCore/`、`src/WtBtCore/`、`src/WtDtCore/` 四处互相重复的 `wtp::` 类定义（`ActionPolicyMgr`、`WtHelper`、`ParserAdapter`、`ParserAdapterMgr`、`TraderAdapter`、`TraderAdapterMgr`、`EventNotifier`、`IParserStub`、`ITrdNotifySink`、`IBaseDataMgr`、`IHotMgr`）
- 相关测试: `n01_wtcore_tradepaths/n01_action_policy_test.cpp` 与 `n03_uftcore_index/n03_action_policy_test.cpp`（同名类的两个 lane）
- 输入/前置条件: 最终链接命令同时提供包含同名强符号的核心实现，并且测试/产品代码引用到该符号。注意：`CoverageTests` 主目标不是 `WtCore.a + WtUftCore.a` 组合；其 CMake 在 L474 显式列出 `WtDtCore` 和 `WtCore`。各测试目标还可能通过共享库的传递链接引入核心归档，须以对应目标的最终链接命令为准。
- 期望行为: 每个模块的实现都能被独立链接与独立度量，或链接期就报重复符号
- 实际行为: **两个归档都定义同一个强符号** `T __ZN3wtp15ActionPolicyMgr4initEPKc`（本机 `nm` 对两份归档均命中）。静态归档按需抽取；若两份实现都被抽入同一链接单元，可能报重复符号；若仅一份满足引用，另一份不会仅因归档中存在同名符号而自动进入二进制。因此，当前符号输出确认了重复定义风险，但尚未单独证明某个 lane 的覆盖归属已经错误；还需目标级链接映射或 `-why_load` 证据确认哪些对象实际进入二进制。
- 复现步骤: `nm -g -arch arm64 build-cov/build_x64/Debug/libs/libWtCore.a | grep ActionPolicyMgr4init`；同命令换 `libWtUftCore.a`。两者都命中即为重复定义。
- 影响范围: 普查到 **37 个跨目录同名 `wtp::` 类**（该数量为历史静态扫描结果，需保留扫描脚本/清单以便复核）。这构成同一程序中重复定义及模块归因风险；是否影响某个生产/覆盖目标，取决于实际编译 TU 与最终抽取的归档成员，不能仅由归档清单推定。
- 备注: 测试侧目标应按每个最终链接单元核对核心来源，避免同名类实现同时编译/抽取；当前 `${WT_COV_SHARED_LIBS}` 中的共享库会带来传递核心依赖，不能只看 CMake 里手写的核心参数。产品侧可评估为领域核心加 namespace，或抽出唯一公共库。**本迭代未修改任何生产代码**；只有在确认目标的真实源码/归档归属后，才能决定是否新增覆盖排除项。

- 复核结论（2026-10-29）: **归档重复强符号已确认；原文的目标链接描述需更正，覆盖归因结论仍待目标级证据。** 本机生成链接命令显示 `CoverageTests_uftcore` 直接编译 `WtUftCore/*.cpp` 并通过依赖引入 `libWtCore.a`，但不含 `libWtUftCore.a`；`CoverageTests_m_WtUftRunner` 的链接行则包含 `libWtUftCore.a` 和经共享依赖带入的 `libWtCore.a`。`CoverageTests` 主目标不是 `libWtCore.a + libWtUftCore.a` 组合。建议为声称受影响的具体目标保留完整链接命令、归档成员抽取证据和 llvm-cov 按源文件报告，再决定 Blocker 严重级别；本次未运行这些目标，也未验证运行时归因。

## BUG-L00-006

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
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

- 复核结论（2026-10-29）: **双版本环境和测试侧桥接路径已由本机现状确认；历史链接失败与修复后退出码本次未重跑。** 当前 cache 选择 Homebrew Boost 1.88，本机 `/usr/local/include/boost` 为 1.81；测试目标生成命令将 `cov_boost_include` 放在 `-isystem /opt/homebrew/include` 前，桥接符号链接指向 Homebrew 头目录，生产 `WtDtHelper` 命令使用 `-I/opt/homebrew/include`。此证据支持当前搜索路径设计，不证明所有覆盖目标或其它机器一致。建议保留头版本宏、链接库真实路径和对象未定义符号三项验证；产品侧可将 Boost 头/库绑定到同一 imported target，或明确并检查单一版本来源。



- 迭代02复核: TEST。声称的头/库版本错配发生在 WT_ENABLE_COVERAGE 测试目标的 include 搜索顺序；生产目标本身有一致的 Boost 选择记录。当前测试 CMake 已加入路径桥接规避。旧链接失败/成功结果未在本轮重跑，所以分类为测试构建配置问题，不是产品运行缺陷。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-L00-007

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Blocker（构建完整性）
- 模块/文件: `src/TestUnits/coverage/CMakeLists.txt` 的 `WT_MAIN_MODULES` 循环（m_<Module> 目标），涉及 `src/WtUftRunner/CMakeLists.txt:21-22`、`src/WtLatencyUFT/CMakeLists.txt:21-22`
- 相关测试: `m_WtUftRunner/r14_wtuftrunner_main_test.cpp`、`m_WtLatencyUFT/r14_uftlatency_main_test.cpp`
- 输入/前置条件: 为带 `main()` 的可执行体建覆盖率测试目标（编译期 `main=wt_coverage_main_<mod>` 重命名），目标需链接与产品一致的核心静态库；早期版本只链 `WT_COV_SHARED_LIBS`。
- 期望行为: 测试目标链接的「核心静态库」应与产品可执行体自己的一致
- 实际行为: 历史记录称早期 `CoverageTests_m_WtUftRunner` 与 `CoverageTests_m_WtLatencyUFT` 因缺少 `WtUftCore` 出现未定义符号；当前 CMake 已从各模块的 CMakeLists 解析核心库并加入目标链接。关于早期 `m_WtRunner`、`m_QuoteFactory`、`m_WtLatencyHFT` 链接成功的解释是历史推断，仍需链接映射确认是否依赖共享库导出。
- 复现步骤: `cmake --build build-cov --target CoverageTests_m_WtUftRunner`（修复前 exit=1，报上面那组未定义符号）
- 影响范围: 配置中的候选列表有 7 个 `m_<Module>`；只有含生产 TU 且有测试 `.cpp` 的候选才会生成目标。链接成功只证明该链接命令通过，不证明所有生产依赖都已插桩或运行分支已覆盖。
- 备注: 测试侧当前循环从模块 CMakeLists 扫描核心库名并加入链接。此规则解决早期缺少目标库的问题，但最终链接仍可能经 `${WT_COV_SHARED_LIBS}` 传递引入其它核心，须与 BUG-L00-005 一起审计。对象库可以让生产可执行体与测试目标共享明确的对象来源，但不是本条唯一修复方向。

- 复核结论（2026-10-29）: **早期缺库问题的测试侧修正已由当前源码和生成链接命令支持；五目标/七目标通过说法未闭环。** 当前构建目录有 6 个 `CoverageTests_m_*` 可执行文件：LoaderRunner、QuoteFactory、WtLatencyHFT、WtLatencyUFT、WtRunner、WtUftRunner；未发现 `m_WtBtRunner` 可执行文件。当前 `m_WtUftRunner` 链接行含 `libWtUftCore.a`，同时也经共享依赖带入 `libWtCore.a`。原备注列出四个目标名却称“五个目标通过”；本次没有重跑任一链接命令。建议按实际生成的 6 个目标逐个记录目标列表、链接退出码、核心归档和未定义符号；空目录/未生成目标标为未覆盖或未运行，不计作通过。


- 迭代02复核: TEST。候选问题是带 main 的测试目标未链接产品可执行体的核心库；当前 coverage CMake 已从模块 CMakeLists 收集核心库并链接。属于测试目标设置问题，历史构建结果未在本轮重跑。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
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

- 复核结论（2026-10-29）: **头文件的多 TU ODR 风险已由源码确认；本次未重跑旧链接失败。** 当前 `ModuleHelper.hpp` 有外部链接的非 inline 定义，覆盖 CMake 保留按源文件哈希改名的测试侧绕法；当前产品中 `WtDtPorter`、`WtDtServo` 是不同共享库，未证明有现存产品目标把多个包含者合并链接。`TraderDD.cpp`、`TraderHTS.cpp` 各有同名定义，但当前未发现它们包含 `ModuleHelper.hpp`，故不应描述为当前同一 TU 的重复定义。建议将头内函数改为具内部链接的定义（如 `static`/匿名命名空间），保留每 TU 的 `dladdr` 锚点语义；评估后再清理两个独立定义。产品运行时影响仍待具体合并目标证据。

## BUG-L00-009

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
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

- 复核结论（2026-10-29）: **初始化顺序告警由源码确认；当前构建范围不支持“告警导致四个模块当前构建失败”的表述。** `ParserYD` 不在顶层 `ADD_SUBDIRECTORY`，本机 `build-cov` 没有 `CoverageTests_t_ParserYD` 目标，当前覆盖目录也没有 `t_ParserYD`；本次未运行编译器复现。声明顺序与列表顺序不一致，但两项初值为常量，当前运行时危害未证实。需把“未进入构建图/覆盖分母”与“如果纳入严格告警构建会失败”区分，并为其它模块分别提供真实编译命令、告警日志及行数来源。建议先由产品 owner 决定是否纳入构建，再选择修正告警或对指定测试 TU 放宽；不要把未构建模块记为测试通过或仅因未覆盖就排除。
