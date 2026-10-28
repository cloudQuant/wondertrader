# N02 — WtCore 后半（引擎/执行/数据管理）测试清单

## 1. 范围
- 模块：`src/WtCore`（STATIC 库，内部 C++ 符号可直接链接，见 `src/WtCore/CMakeLists.txt:23`）
- 本 lane 负责的文件：`WtCtaEngine`、`WtCtaTicker`、`WtDiffExecuter`、`WtDistExecuter`、`WtDtMgr`、
  `WtEngine`、`WtExecMgr`、`WtExecuterFactory`、`WtFilterMgr`、`WtHelper`、`WtHftEngine`、
  `WtHftTicker`、`WtLocalExecuter`、`WtSelEngine`、`WtSelTicker` 及对应 `.h`
  （前半 `Cta/Sel/HftStraBaseCtx`、`TraderAdapter`、`ParserAdapter` 等属 N01，未触碰）
- 测试目录：`src/TestUnits/coverage/n02_wtcore_engines/`
- 简报：`docs/迭代计划/迭代01/manifests/api-brief-L07.md`

## 2. 覆盖情况

### 已覆盖的公开 API/函数
| 源文件 | 覆盖符号 |
|---|---|
| `WtHelper.cpp/.h` | `getCWD`、`getModulePath`（isCWD true/false 两支）、`getBaseDir`、`getOutputDir`、`getStraDataDir`、`getStraUsrDatDir`、`getPortifolioDir`、`getExecDataDir`、`setGenerateDir`、`setInstDir`/`getInstDir`、`setTime`/`getDate`/`getTime`/`getSecs`、`setTDate`/`getTradingDate` |
| `WtDistExecuter.cpp` | ctor、dtor（经基类 shared_ptr）、`init`（NULL / 有 scale / 无 scale 三支）、`set_position`（首值、重复值 `decimal::eq`、归零、空 map）、`on_position_changed`（缩放、变化、不变）、`on_tick` 空实现 |
| `WtFilterMgr.cpp` | `load_filters`（无文件早退、文件不存在、首次装配、mtime 未变跳过、mtime 前进后重载并清空重建三张表）、`is_filtered_by_strategy`（ignore / redirect / isDiff 直丢 / 未命中）、`is_filtered_by_code`（精确代码命中、`CodeInfo::stdCommID()` 品种级回退、未命中）、`is_filtered_by_executer`（true/false/未配置）、action 大小写不敏感与未识别 action 跳过 |
| `WtExecMgr.cpp/.h` | `add_executer`（含同名覆盖）、`enum_executer`、`set_positions`（无过滤 / 代码 ignore 剔除 / 代码 redirect 改写 / 通道 banned 跳过）、`handle_pos_change`（`"ALL"` 广播、命中 `_routed_executers` 时跳过、显式 execid 精确路由、ignore 直接 return、redirect 平移 diff）、`handle_tick`、`add_target_to_cache`（累加/分组）、`commit_cached_targets`（缩放、符号保持、0 跳过、私有组优先于 `"ALL"`、无人认领组 continue、过滤器路径、提交后清缓存）、`clear_cached_targets`、`load_router_rules`（NULL / 非数组 / executer 为字符串 / executer 为数组）、`get_route_by_strategy`（无规则→`{ALL}`、命中规则、未登记→`{ALL}`）、`set_filter_mgr` |
| `WtExecuterFactory.cpp/.h` | `loadFactories`（目录不存在→false、空目录→true、跳过子目录与非 `.dylib`、dlopen 失败 continue、传入普通文件抛 `filesystem_error`）、`createExeUnit`/`createDiffExeUnit`/`createArbiExeUnit` 两个重载的"工厂未注册"与"名字切分不足两段"与"三段名取前两段"分支、`ExeUnitWrapper`（ctor/`self()`/dtor 经工厂 `deleteExeUnit` 回收、shared_ptr 双引用只释放一次、NULL unit 不触碰工厂）、`ExecuteDefs.h` 的 `ExecuteUnit` 回调面（`set_position`/`on_tick`/`on_trade`/`on_order`/`on_entrust`/`on_channel_ready`/`on_channel_lost`）与 `IExecuterFact` 替身 |
| `WtCtaTicker.h/.cpp`、`WtSelTicker.cpp`、`WtHftTicker.cpp` | `WtCtaRtTicker`/`WtSelRtTicker`/`WtHftRtTicker` 构造函数、`on_tick` 的 `_thrd == NULL` → `trigger_price` 短路支、`trigger_price` 的 `_engine` 判空支、`stop()`（未 run 时幂等） |
| `IExecCommand.h` | `name()`/`setName()`/`setStub()`、`set_position`/`on_position_changed`/`on_tick` 默认空实现经基类接口调用 |

### 已覆盖的分支/边界
- 空指针入参：`init(nullptr)`、`load_router_rules(NULL)`、`on_tick(code, NULL)`、`create*` 的替身返回 NULL。
- 幂等与重复：同 mtime 二次 `load_filters`、重复 `add_executer`、重复 `set_position`、重复 `stop()`。
- 数值改写语义：redirect 改写目标仓位、diff 场景直丢、缩放符号保持、0 仓位跳过缩放、私有执行器组优先于 `"ALL"`。
- 文件系统边界：不存在的路径、空目录、目录名带 `.dylib` 后缀、非 Mach-O 的 `.dylib`、把文件当目录传参。

## 3. 测试清单
| 测试文件 | 测试用例数 | 覆盖目标 | 运行时长 |
|---|---:|---|---:|
| `n02_helper_dist_executer_test.cpp` | 12 | `WtHelper` 全部静态工具 + `WtDistExecuter` + `IExecCommand` 接口 | <0.1s（未实测，纯内存与 mkdir） |
| `n02_filter_mgr_test.cpp` | 13（含 1 个 `DISABLED_`） | `WtFilterMgr` 三类过滤器装配/热重载/查询 | <0.1s |
| `n02_exec_mgr_test.cpp` | 17（含 1 个 `DISABLED_`） | `WtExecuterMgr` 路由/过滤/缓存提交 | <0.1s |
| `n02_executer_factory_test.cpp` | 14 | `WtExecuterFactory::loadFactories` + `create*` 失败支 + `ExeUnitWrapper` 生命周期 | <0.2s（含 dlopen 失败尝试） |
| `n02_ticker_slicing_test.cpp` | 7（含 1 个 `DISABLED_`） | 三个 RtTicker 构造 / `on_tick` 短路 / `stop` | <0.1s |
| 合计 | **63（60 启用 + 3 `DISABLED_`）** | | |

行数（wc -l）：511 / 319 / 391 / 295 / 122，均在 150–400~511 区间（`n02_ticker_slicing_test.cpp` 偏短，
因为 ticker 可安全测的接口面确实很小）。

## 4. 依赖与 mock
- fixture：每个文件自带 `TempRoot`，落盘根目录 = `std::filesystem::temp_directory_path()/n02_<tag>_<pid>`，
  `SetUp` 建、`TearDown` 整体 `remove_all`；用例之间零共享状态（可一用例一进程并行）。
- 替身：`RecExecuter : IExecCommand`（记录收到的目标仓位表/仓位变动/tick）、
  `FakeExecuteUnit : ExecuteUnit` + `FakeExecuterFact : IExecuterFact`（记录 `deleteExeUnit` 调用次数）。
- 配置装载：统一用工程自身的 `WTSCfgLoader::load_from_file`（`WtFilterMgr.cpp:38` 同源路径），
  不引入 yaml-cpp 依赖，配置文件均为临时 JSON。
- 时间：不启动 ticker 线程，不断言墙钟/本地时区；热重载用 `fs::last_write_time(p, old + 5s)` 显式推进 mtime，
  避免 sleep 与时区敏感断言。
- 无网络、无 CTP、无 DAT_SERV 端口占用。

## 5. 缺陷
`docs/迭代计划/迭代01/bugs/n02_wtcore_engines.md`，共 **6 条，状态全部「待审核」**：
BUG-N02-001（`load_filters` 解析结果未判空 → 坏配置崩溃）、BUG-N02-002（三个 RtTicker `_s_info/_store` 未初始化 → UB，Blocker）、
BUG-N02-003（`IExecCommand` 虚函数族 + 非虚析构 → 基类指针 delete 为 UB）、BUG-N02-004（`load_router_rules`
的 `item`/`itemExec` 未判空）、BUG-N02-005（`WtDistExecuter` 成员未初始化 + `_config` retain 不 release）、
BUG-N02-006（`loadFactories` 对普通文件路径抛未捕获异常）。
对应缺陷类输入已全部收进同名的 3 个 `DISABLED_` 用例（每类一个，启用用例里不留同类输入）。

## 6. 未覆盖目标及原因（本 lane 范围文件）
| 目标 | 原因 | 需要的支撑（写清给主控） |
|---|---|---|
| `WtEngine`、`WtCtaEngine`、`WtHftEngine`、`WtSelEngine` | 构造即拉起 `WtDtMgr`/`TraderAdapter`/socket 服务端与 `IBaseDataMgr`，无依赖注入点；`init/run/stop` 需要真实行情服务与交易柜台连接 | 需要可注入的 stub 引擎/数据管理器，或允许起本地 DAT_SERV 端口 |
| `WtLocalExecuter`、`WtDiffExecuter`（下单/撮合/拆单） | 二者只做"目标仓位→插件 ExecuteUnit"的分发：`buy/sell/cancel/getPosition/getUndoneQty/getTicks/getCommodityInfo/getSessionInfo/getCurTime` 全部经 `_trader`(TraderAdapter)、`_data_mgr`、`_bd_mgr` 裸指针，NULL 即崩；`on_trade/on_order/on_position/on_entrust/on_channel_ready/on_account` 依赖 `TraderAdapter` 事件环；真正的拆单算法在 `src/WtExeFact/*` 的 ExecuteUnit 里（不在本 lane 文件清单，且属 hidden-visibility SHARED 插件） | ①建议主控把 `src/WtExeFact/*.cpp` 的拆单用例排进独立 lane（`CoverageTests` 已 GLOB 这些 TU，无需新 target）；②本 lane 若要覆盖两个 Executer，需要 fake `TraderAdapter`/`IDataManager`/`IBaseDataMgr` 注入点 |
| `WtDtMgr`（订阅 key 组合与重复订阅抑制） | 订阅/去重路径在 `init/initStore/get_kline_slice` 内，依赖 `WtDtServoApi` 真实端口 + `IBaseDataMgr` 实例；本轮预算内未逐行核读该文件 | 需本地 dt servo stub 或 `IHisDataLoader` 注入（`regsiter_loader` 是唯一注入口，但订阅链仍需 store） |
| `WtExecuterFactory::create*` 的"工厂已加载"成功支、`loadFactories` 的 dlopen 成功支 | 需要真实可 dlopen 的执行器插件 `.dylib`，本 lane 禁止触发构建 | 覆盖率构建产出插件 dylib 后，由集成阶段指向 `build/**/libexecuters` 目录即可补齐 |
| 三个 RtTicker 的 `run()` 线程 + bar 切片推进（`_cur_pos/_last_emit_pos` 分钟推进、小节结束、跨日 `getNextDate`） | `init()` 首行即 `_engine->get_session_info()`，`run()` 依赖 `get_basedata_mgr()->calcTradingDate()` 与 `TimeUtils::getLocalTimeNow()` 墙钟驱动线程 | 需要假 `Wt*Engine` + 可构造的 `WTSSessionInfo`；且属墙钟敏感代码，按本轮纪律不断言时间 |
| `WtFilterMgr` 的 `_notifier != NULL` 通知支、`WtEngine/Wt*Engine` 的事件上报 | `EventNotifier` 需 ZMQ 端点，属外部依赖 | 集成阶段起本地 inproc endpoint |

## 7. 验证（门禁）
逐文件执行且只执行下面这一条命令（从仓库根）：
```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/n02_wtcore_engines/<file>.cpp
```
结果：**5/5 文件 0 error 0 warning**（最后一次整目录循环无任何诊断输出，`grep -c '^TEST'` = 12/13/17/14/7）。
未跑 cmake / ninja / 全量构建 / 测试二进制。
过程中因门禁暴露的两处真实问题：①`delete` 通过 `IExecCommand*`（→ BUG-N02-003）；
②`shared_ptr<RecExecuter>` 回收同样命中该告警（替身补虚析构）。均未放宽断言、未改生产代码。

### 给主控的备注
- 本 lane 未新增 CMake 目标，`src/TestUnits/coverage/CMakeLists.txt` 的 `GLOB_RECURSE` 会自动收进
  `n02_wtcore_engines/*.cpp`；WtCore 是 STATIC 库，符号可直接链接，无需把它的 TU 追加进 `PLUGIN_TUNS`。
- 剩余可补项按性价比排序：`WtExeFact` 拆单算法（独立 lane）> 两个 Executer 的 fake TraderAdapter >
  `WtDtMgr` 订阅链（需本地 dt servo）> 引擎主循环。
