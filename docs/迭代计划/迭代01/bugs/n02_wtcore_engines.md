# lane N02 — WtCore（引擎/执行/数据管理）缺陷记录

> 以下为源码与测试前提复核结果；本迭代不修改生产代码。运行证据仅在明确说明处列出。模板见 `README.md`。
> 涉及测试目录：`src/TestUnits/coverage/n02_wtcore_engines/`

## BUG-N02-001

- 状态: 真实缺陷（源码核实；本轮未运行崩溃路径）
- 严重级别: Major
- 模块/文件: `src/WtCore/WtFilterMgr.cpp:38-47`（`WtFilterMgr::load_filters`）
- 相关测试: `n02_filter_mgr_test.cpp` → `N02FilterMgrDisabled.DISABLED_MalformedConfigFilesCrashLoadFilters`
- 输入/前置条件: 过滤器文件存在但为空、JSON 语法错误，或顶层为数组。`WTSCfgLoader::load_from_file()` 对这些输入返回 NULL；其中顶层数组因 loader 将根节点转换到 object 类型失败而返回 NULL。
- 期望行为: 记录错误日志并保持既有过滤器表不变后返回。
- 实际行为: loader 返回 NULL 后，函数更新时间戳、清空现有过滤器表，再执行 `cfg->get("strategy_filters")`，对 NULL 的 `this` 解引用。`{"strategy_filters": null}` 不属于该崩溃输入：JSON 转换器会省略 null 成员，根对象仍有效，后续 `get` 返回 NULL 并被守卫。
- 复现步骤: 把 `filters.json` 写成空文件，调用 `WtFilterMgr::load_filters("filters.json")`。
- 影响范围: 运行期热加载过滤器时，空文件或解析失败会把控制流带到空配置解引用；本轮未追踪具体生产调用频率，也未发现 `WtUftCore` 存在同名加载器实现。
- 误报输入状态: 已撤回（`{"strategy_filters": null}` 不会令 loader 返回 NULL）。
- 备注: `DISABLED_` 用例未运行，且首个空文件样例一旦崩溃便不会继续验证其它输入。启用用例只喂合法 JSON。建议在改动时间戳和清空旧表之前判 `cfg == NULL` 并记错后返回；节点为空已有守卫。不实施。

## BUG-N02-002

- 状态: 待验证（成员未初始化已确认；构造后、init 前调用是否违反生命周期契约未确认）
- 严重级别: Major
- 模块/文件: `src/WtCore/WtCtaTicker.h:41-48`、`src/WtCore/WtSelTicker.cpp:26-35`、
  `src/WtCore/WtHftTicker.cpp:26-35`（三个 RtTicker 的构造函数）
- 相关测试: `n02_ticker_slicing_test.cpp` → `N02TickerDisabled.DISABLED_UninitialisedSessionInfoIsReadBeforeInit`
- 输入/前置条件: 已构造 `WtCtaRtTicker`，尚未调用 `init()` 即调用公开的 `is_in_trading()` 或 `time_to_mins()`。Sel/HFT 只有在 `init()` 前调用 `run()` 等触达 session 路径时才会读 `_s_info`；这一路径是否属于受支持调用顺序，本轮未从接口契约确认。
- 期望行为: ticker 成员在构造时具有确定的空值状态；若查询允许在 `init()` 前调用，应分别返回 false 和原样时间。
- 实际行为: 三个构造函数均未初始化 `_s_info` / `_store`。CTA 两个公开查询会先比较未初始化的 `_s_info`，形成未定义行为；若残值非空再解引用。Sel/HFT 的无线程 `on_tick()` 快路径不会读这两个成员，不能据此认定常规回调会触发；它们的 `run()` 在初始化前会访问 `_engine` 和 `_s_info`。
- 复现步骤: CTA 源码路径可静态确认：构造 `WtCtaRtTicker t(nullptr)` 后调用 `t.is_in_trading()` 或 `t.time_to_mins(93000)`。该未定义行为的具体运行表现未验证。
- 影响范围: 若调用者在初始化前调用 CTA 查询，或对 Sel/HFT 在初始化前调用 `run()`，未初始化成员可导致未定义行为。三个引擎源码调用顺序均先 `ticker->init(...)` 再 `ticker->run()`；本轮没有证据表明正常引擎路径会命中。
- 备注: 禁用用例先调用 CTA 查询，Sel/HFT 部分仅以空引擎调用无线程 `on_tick()` / `stop()`，没有读取 `_s_info` / `_store`，不能证明三个 ticker 都会崩溃。是否要求 standalone ticker 支持 init 前查询需结合接口契约确认。建议初始化成员为空，并在需要初始化的入口校验生命周期。不实施。

## BUG-N02-003

- 状态: 真实接口缺陷（源码核实；裸基类删除路径未运行）
- 严重级别: Major
- 模块/文件: `src/WtCore/IExecCommand.h:61-115`
- 相关测试: `n02_helper_dist_executer_test.cpp` →
  `N02DistExecuterTest.ExecuterIsUsableThroughIExecCommandBase`（只验证基类接口调用；没有基类指针删除断言）
- 输入/前置条件: 以 `IExecCommand*` 持有 `WtLocalExecuter` / `WtDiffExecuter` / `WtDistExecuter`
  等派生执行器并 `delete`。
- 期望行为: 基类析构函数为 virtual，通过基类指针删除多态对象是良定义的。
- 实际行为: `IExecCommand` 有 virtual 业务方法但没有 virtual 析构函数；通过 `IExecCommand*` 删除派生实例不符合 C++ 多态删除规则。当前检索到的 runner 用 `ExecCmdPtr(派生类指针)` 构造 shared_ptr，控制块保留派生类型的删除器；不能据此声称这些在库内路径已触发缺陷。风险适用于直接基类指针删除或以基类静态类型创建默认删除器的外部调用方。
- 复现步骤: `IExecCommand* p = new WtDistExecuter("x"); delete p;` 是源代码层面的反例；该语句未在本轮编译或运行。当前 `ExecuterIsUsableThroughIExecCommandBase` 仅通过基类接口调用对象，并由 `shared_ptr<WtDistExecuter>` 持有，不验证基类析构。
- 影响范围: 执行器生命周期管理；与 `WtDistExecuter` 析构不释放 `_config`（BUG-N02-005）叠加会放大泄漏。
- 备注: 原测试备注所称“clang 诊断直接报错、测试被迫使用具体类型 shared_ptr”没有本轮构建证据；检查到的测试源只证明基类接口可调用。建议修复方向：`virtual ~IExecCommand() {}`。不实施。

## BUG-N02-004

- 状态: 真实缺陷（缺少/null executer 会解引用空指针；本轮未运行）；误报子项已撤回
- 严重级别: Major
- 模块/文件: `src/WtCore/WtExecMgr.cpp:209-242`（`WtExecuterMgr::load_router_rules`）
- 相关测试: `n02_exec_mgr_test.cpp` → `N02ExecMgrDisabled.DISABLED_RouterRuleWithoutExecuterNodeCrashes`
- 输入/前置条件: `load_router_rules()` 收到有效数组，其中某条规则缺少 `executer` 节点，或该成员为 JSON null。`strategy` 缺失不是这里描述的空指针崩溃输入。
- 期望行为: 跳过该条规则并记录错误，函数返回 false 或忽略。
- 实际行为: 在正常对象条目中，缺少或为 null 的 `executer` 会使 `itemExec == NULL`，随后 `itemExec->isArray()` 解引用。缺少 `strategy` 时 `getCString()` 返回默认空串，不会在该处崩溃；会产生空策略名规则，是否违反路由契约待确认。
- 复现步骤: 将一条缺少 `executer` 的规则放在传给函数的数组中，例如从对象配置的嵌套数组成员取出后调用。原禁用用例把顶层数组交给 loader 导致返回 NULL、未触达目标函数；现已修正为从有效对象提取嵌套数组。
- 影响范围: 启动期的执行器路由装配；一处配置笔误即导致交易进程启动失败。
- 误报子项状态: 已撤回（“缺少 strategy 导致 NULL 解引用”）。
- 备注: 修正后的缺 `executer` 用例仍为禁用，本轮未执行崩溃路径；缺 `strategy` 的独立控制测试已通过，仅确认该处不崩溃。建议为 `item` / `itemExec` 和必需字段加校验。不实施。

## BUG-N02-005

- 状态: 真实缺陷（成功 init 后 `_config` 引用泄漏）；未初始化 `_scale` 的 init 前使用风险待验证
- 严重级别: Minor
- 模块/文件: `src/WtCore/WtDistExecuter.cpp:26-58`（构造函数 / `init` / 析构函数）
- 相关测试: `n02_helper_dist_executer_test.cpp` →
  `N02DistExecuterTest.InitSucceedsWithoutScaleNode`、`InitRejectsNullParams`
- 输入/前置条件: 真实缺陷的确定路径为 `init(cfg)` 成功后销毁对象；另一个风险路径是在构造或 `init(nullptr)` 失败后调用仓位方法。
- 期望行为: 成功 `init` 所 retain 的配置引用应由对象释放。若接口允许初始化前调用仓位方法，`_scale` 也需确定并拒绝未初始化调用；缺省 scale 的契约应沿用现有配置语义，不应直接推定为 1。
- 实际行为: 构造函数不初始化 `_config` / `_scale`。成功 `init` 会将 `_config` retain 一次并写入 `_scale`，但析构函数为空，因此配置引用泄漏。未指定 `scale` 时 `getUInt32` 默认返回 0，测试已有此预期；`_scale` 未定义只发生在成功 init 前使用仓位方法的情况下。
- 复现步骤: 引用计数路径可由 `init(cfg)`、调用方释放原引用、执行器析构静态确认；本轮未运行泄漏检测。`InitRejectsNullParams` 后没有调用仓位方法，不能证明 `_scale` 的运行结果。
- 影响范围: 每个成功初始化后析构的 `WtDistExecuter` 保留一份配置引用。未初始化仓位调用风险依赖违反 `init` 生命周期；检索到的 runner 路径先成功调用 `init(cfgItem)` 再注册执行器。
- 备注: `InitSucceedsWithoutScaleNode` 证实缺省 scale 为 0，不支持将该行为判作缺陷；两个现有测试都未验证配置引用释放。建议析构中释放 `_config`；若保留未初始化调用问题，应先明确接口生命周期并加状态检查。不实施。

## BUG-N02-006

- 状态: 真实异常传播缺陷（源码/标准行为核实；本轮未运行测试）
- 严重级别: Minor
- 模块/文件: `src/WtCore/WtExecuterFactory.cpp:15-25`（`WtExecuterFactory::loadFactories`）
- 相关测试: `n02_executer_factory_test.cpp` →
  `N02ExecuterFactoryTest.LoadFactoriesWithRegularFileThrows`（启用用例；本轮未运行）
- 输入/前置条件: `loadFactories(path)` 的 `path` 指向一个**普通文件**而不是目录。
- 期望行为: 返回 false 并记录错误（与目录不存在同样的容错路径）。
- 实际行为: `StdFile::exists()` 对普通文件返回 true，随后
  `fs::directory_iterator(myPath)` 抛 `std::filesystem::filesystem_error`；函数本身不捕获，异常会向调用栈传播。调用方的最终异常处理行为本轮未核实。
- 复现步骤: `WtExecuterFactory f; f.loadFactories("<某个存在的文件>");`
- 影响范围: 配置里把执行器插件目录写成普通文件路径时会使加载调用抛异常；是否最终导致进程退出取决于调用方捕获策略，本轮未运行该启动路径。
- 备注: 启用测试 `LoadFactoriesWithRegularFileThrows` 与源码描述一致，但本轮未运行，不能记为运行证据。建议先 `fs::is_directory(path)` 再遍历，或使用带 `error_code` 的重载。同类写法在 `CtaStrategyMgr.cpp:39`、`WtParserFact` 装载等处也存在（属 N01 范围，此处仅点名）。
