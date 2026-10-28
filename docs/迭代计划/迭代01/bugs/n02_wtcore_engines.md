# lane N02 — WtCore（引擎/执行/数据管理）缺陷记录

> 状态一律「待审核」；本迭代不修改生产代码。模板见 `README.md`。
> 涉及测试目录：`src/TestUnits/coverage/n02_wtcore_engines/`

## BUG-N02-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtCore/WtFilterMgr.cpp:38-47`（`WtFilterMgr::load_filters`）
- 相关测试: `n02_filter_mgr_test.cpp` → `N02FilterMgrDisabled.DISABLED_MalformedConfigFilesCrashLoadFilters`
- 输入/前置条件: 过滤器文件存在但内容非法，使 `WTSCfgLoader::load_from_file()` 返回 NULL
  （空文件、截断的 `{`、顶层为数组、完全非 JSON、`{"strategy_filters": null}`）。
- 期望行为: 记录错误日志并保持既有过滤器表不变后返回。
- 实际行为: 紧接着执行 `cfg->get("strategy_filters")`，`WTSVariant::get` 先读 `_type` 再读
  `_value._map`，对 NULL 的 `this` 解引用 → 段错误。整个 `load_filters` 只校验了文件是否存在，
  没有校验解析结果。
- 复现步骤: 把 `filters.json` 写成空文件，调用 `WtFilterMgr::load_filters("filters.json")`。
- 影响范围: 运行期热加载过滤器（`on_all_filtered_updated` 之类的定时重载）时，一次误编辑的
  配置文件即可让交易进程崩溃；`WtCore` / `WtUftCore` 两份实现同构。
- 备注: 阻塞过滤器解析异常路径的覆盖率（该缺陷类输入已全部收进唯一一个 `DISABLED_` 用例，
  启用用例只喂合法 JSON）。建议修复方向：`if (cfg == NULL) { WTSLogger::error(...); return; }`，
  并对 `cfg->get(...)` 的子节点同样判空。不实施。

## BUG-N02-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtCore/WtCtaTicker.h:41-48`、`src/WtCore/WtSelTicker.cpp:26-35`、
  `src/WtCore/WtHftTicker.cpp:26-35`（三个 RtTicker 的构造函数）
- 相关测试: `n02_ticker_slicing_test.cpp` → `N02TickerDisabled.DISABLED_UninitialisedSessionInfoIsReadBeforeInit`
- 输入/前置条件: 构造 `WtCtaRtTicker/WtSelRtTicker/WtHftRtTicker` 后，在 `init()` 之前调用
  `is_in_trading()`、`time_to_mins()`，或任何读取 `_s_info` / `_store` 的路径。
- 期望行为: 成员 `WTSSessionInfo* _s_info` / `IDataReader* _store` 在构造时置空，
  未 init 时 `is_in_trading()` 返回 false、`time_to_mins()` 原样返回入参。
- 实际行为: 三个构造函数初始化列表都只写了 `_engine/_date/_time/_cur_pos/_next_check_time/
  _last_emit_pos/_stopped`，`_s_info` 与 `_store` 是未定义的堆上残值；
  `if (_s_info == NULL) return false;` 这类判空形同虚设，非空即直接 `_s_info->isInTradingTime(...)`
  → 读随机地址（UB/崩溃）。
- 复现步骤: `WtCtaRtTicker t(nullptr); t.is_in_trading();`
- 影响范围: CTA/选股/高频三个实时引擎的 ticker 在 `init()` 失败或提前回调时都可能命中；
  崩溃点随机、难以复现，属于内存安全类缺陷。
- 备注: 阻塞 ticker 未初始化防护分支的覆盖。建议修复方向：把 `_s_info(NULL), _store(NULL)`
  加入三个构造函数的初始化列表。不实施。

## BUG-N02-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtCore/IExecCommand.h:61-115`
- 相关测试: `n02_helper_dist_executer_test.cpp` →
  `N02DistExecuterTest.ExecuterIsUsableThroughIExecCommandBase`（clang 诊断
  `-Wdelete-non-abstract-non-virtual-dtor` 直接报错，测试被迫改用 `shared_ptr<WtDistExecuter>`）
- 输入/前置条件: 以 `IExecCommand*` 持有 `WtLocalExecuter` / `WtDiffExecuter` / `WtDistExecuter`
  等派生执行器并 `delete`。
- 期望行为: 基类析构函数为 virtual，通过基类指针删除多态对象是良定义的。
- 实际行为: `IExecCommand` 有一整组 virtual 业务方法，析构函数却是隐式的非虚函数 →
  `delete` 基类指针是 UB（派生类成员与 `WTSVariant* _config` 不会被回收）。
  工程内 `ExecCmdPtr = shared_ptr<IExecCommand>` 恰好在构造时擦除了具体类型，掩盖了问题；
  任何裸指针写法（例如 `WtExecMgr` 之外的手工管理）都会踩中。
- 复现步骤: `IExecCommand* p = new WtDistExecuter("x"); delete p;` → clang `-Werror` 报错，
  运行期按 UB 处理。
- 影响范围: 执行器生命周期管理；与 `WtDistExecuter` 析构不释放 `_config`（BUG-N02-005）叠加会放大泄漏。
- 备注: 建议修复方向：`virtual ~IExecCommand() {}`。不实施。

## BUG-N02-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtCore/WtExecMgr.cpp:209-242`（`WtExecuterMgr::load_router_rules`）
- 相关测试: `n02_exec_mgr_test.cpp` → `N02ExecMgrDisabled.DISABLED_RouterRuleWithoutExecuterNodeCrashes`
- 输入/前置条件: 路由配置数组中的条目缺少 `executer` 节点、缺少 `strategy` 节点，
  或 `executer` 显式为 `null`（`[{ "strategy": "s" }]` / `[{ "executer": "e" }]` /
  `[{ "strategy": "s", "executer": null }]`）。
- 期望行为: 跳过该条规则并记录错误，函数返回 false 或忽略。
- 实际行为: 循环体里 `item` 与 `itemExec` 都未判空，随后 `itemExec->isArray()` /
  `item->getCString("strategy")` 直接解引用 NULL → 崩溃。
- 复现步骤: 用 `version.json` 的 `executers` 路由段配置一条没有 `executer` 的规则后重启。
- 影响范围: 启动期的执行器路由装配；一处配置笔误即导致交易进程启动失败。
- 备注: 阻塞该函数参数校验分支的覆盖（同类输入收进唯一一个 `DISABLED_` 用例）。
  建议修复方向：对 `item`/`itemExec` 判空并 `continue`。不实施。

## BUG-N02-005

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtCore/WtDistExecuter.cpp:26-58`（构造函数 / `init` / 析构函数）
- 相关测试: `n02_helper_dist_executer_test.cpp` →
  `N02DistExecuterTest.InitSucceedsWithoutScaleNode`、`InitRejectsNullParams`
- 输入/前置条件: 1) 构造 `WtDistExecuter` 后不调用 `init()` 就调用 `set_position()`；
  2) 调用 `init(cfg)` 后析构执行器。
- 期望行为: 1) `_scale` 有确定的默认值（例如 1.0 或 0）；2) 析构时释放构造期 `retain()` 的 `_config`。
- 实际行为: 1) 构造函数体为空，`WTSVariant* _config` 与 `uint32_t _scale` 均未初始化，
  `newVol *= _scale` 读取不确定值；2) `init()` 里 `_config->retain()`，而 `~WtDistExecuter()`
  是空实现，不 `release()` → 引用泄漏。
- 复现步骤: 见上述两个用例（`InitRejectsNullParams` 之后 `_scale` 仍处于不确定状态）。
- 影响范围: 分布式执行器在配置缺失/延迟初始化时的仓位缩放值不确定；长跑进程的配置对象泄漏。
- 备注: 建议修复方向：构造函数初始化 `_config(NULL), _scale(1)`，析构里
  `WTS_SAFE_RELEASE(_config)`。不实施。

## BUG-N02-006

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtCore/WtExecuterFactory.cpp:15-25`（`WtExecuterFactory::loadFactories`）
- 相关测试: `n02_executer_factory_test.cpp` →
  `N02ExecuterFactoryTest.LoadFactoriesWithRegularFileThrows`（启用用例，行为可确定观测）
- 输入/前置条件: `loadFactories(path)` 的 `path` 指向一个**普通文件**而不是目录。
- 期望行为: 返回 false 并记录错误（与目录不存在同样的容错路径）。
- 实际行为: `StdFile::exists()` 对普通文件返回 true，随后
  `fs::directory_iterator(myPath)` 抛 `std::filesystem::filesystem_error`，
  函数与所有调用方都未捕获 → 异常逃出模块边界，进程 terminate。
- 复现步骤: `WtExecuterFactory f; f.loadFactories("<某个存在的文件>");`
- 影响范围: 配置里把执行器插件目录写成文件路径/残留软链时，启动即崩。
- 备注: 建议修复方向：先 `fs::is_directory(path)` 再遍历，或给遍历加 `error_code` 重载。
  不实施。同类写法在 `CtaStrategyMgr.cpp:39`、`WtParserFact` 装载等处也存在（属 N01 范围，此处仅点名）。
