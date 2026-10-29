# p06_uftcore2 待审核缺陷（lane p06 / WtUftCore 剩余部分）

> 本文件只记录，不修改 `src/**`。状态一律「待审核」。
> 与 `docs/迭代计划/迭代01/bugs/n03_uftcore_index.md`（BUG-N03-001…008）同一缺陷类的条目只做引用，不重复立案。

## BUG-P06-001

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/WtUftEngine.cpp:35-51`、`src/WtUftCore/WtUftEngine.h:133-134,154`
- 相关测试: `src/TestUnits/coverage/p06_uftcore2/p06_uft_engine_test.cpp` → `DISABLED_p06_uft_engine.uninitialized_and_malformed_stdcode_class` 第 (1) 段
- 输入/前置条件: `WtUftEngine e;` 之后、`e.init(...)` 之前调用任意查询接口（`get_contract_info` / `get_commodity_info` / `get_session_info` / `get_cur_price` / `get_last_tick`）
- 期望行为: 构造函数把 `_base_data_mgr`、`_data_mgr`、`_dependent` 一并初始化为 `NULL/false`，或在未 init 时安全返回
- 实际行为: 初始化列表只有 `_tm_ticker/_cfg/_notifier`；`_base_data_mgr`、`_data_mgr`、`_dependent` 是未初始化标量/指针，未 init 即读就是野指针解引用
- 复现步骤: 取消 `DISABLED_` 前缀后运行 `CoverageTests_uftcore`，第 (1) 段立刻崩溃
- 影响范围: 所有「先 new 引擎、后 init」的装配路径（WtLatencyUFT/WtUftCore 调用方）；也让引擎的查询接口无法在单测里脱离 init 使用
- 备注: 与 BUG-N03-005（TraderAdapter 未初始化成员）同一缺陷类，修复方向是补齐构造函数初始化列表；不实施。本波启用用例已改为「init 之后再查」，不阻塞覆盖率目标


- 迭代02复核: TEST。候选在 WtUftEngine 构造后、init 前调用依赖基础数据/数据管理器的查询；正常 runner 装配先 init 引擎再启动策略。没有公开契约要求未 init 引擎提供查询能力，故这是生命周期前置错误而非本轮产品 bug。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-P06-002

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/WtUftEngine.cpp:118-168`（`get_commodity_info` / `get_contract_info` / `get_session_info(isCode=true)`）
- 相关测试: 同上 DISABLED 用例第 (2) 段
- 输入/前置条件: 传入不含 `.` 的合约代码，例如 `get_contract_info("600000")`
- 期望行为: 校验 `StrUtil::split` 结果长度，不足两段时返回 `NULL`（或按交易所补全）
- 实际行为: 直接取 `ay[1].c_str()` / `ay[0].c_str()`，`StringVector` 只有 1 个元素时越界读
- 复现步骤: DISABLED 用例第 (2) 段
- 影响范围: 策略侧任何漏写交易所前缀的 `stra_get_*` 调用；越界读可能表现为随机崩溃或脏数据
- 备注: 阻塞该分支的覆盖（无法写启用断言）；建议修复方向为统一的 `extractStdCode` 校验


- 迭代02复核: TEST。WtUftEngine 的查询接口注释要求标准化交易所.合约代码；用 600000 缺交易所段，且测试和正常查询契约不符。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-P06-003

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Major
- 模块/文件: `src/WtUftCore/WtUftEngine.cpp:161-167`
- 相关测试: 同上 DISABLED 用例第 (3) 段
- 输入/前置条件: `get_session_info("SSE.600000", true)`，合约存在但 `cInfo->getCommInfo()` 为 `NULL`
- 期望行为: 与 `get_commodity_info` 一致，先判空再取 `getSessionInfo()`
- 实际行为: `commInfo->getSessionInfo()` 未判空，直接解引用
- 复现步骤: DISABLED 用例第 (3) 段
- 影响范围: 基础数据未完整挂接（合约/品种分离装载）时的启动早期查询
- 备注: 不阻塞覆盖率目标（该分支已收进 DISABLED）

## BUG-P06-004

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/UftStraContext.cpp:43-50`、`src/WtUftCore/UftStraContext.h`（`_trader` 成员）
- 相关测试: `src/TestUnits/coverage/p06_uftcore2/p06_uft_context_test.cpp` → `DISABLED_p06_uft_context.uninitialized_trader_and_param_class` 第 (1)(2) 段
- 输入/前置条件: `UftStraContext ctx(&engine, "x");` 后未调用 `setTrader()`，直接 `stra_get_position` / `stra_cancel` / `stra_get_undone`；或 `setTrader()` 挂一个从未 `init()` 的 `TraderAdapter`
- 期望行为: 构造函数把 `_trader` 初始化为 `NULL`，下单/查询接口判空后安全返回
- 实际行为: 初始化列表只有 `IUftStraCtx(name)/_engine/_tradingday/_strategy`，`_trader` 未初始化；`stra_get_position` 等 8 个接口无判空地解引用
- 复现步骤: DISABLED 用例第 (1)(2) 段（第 (2) 段同时触发 BUG-N03-005 的适配器空表路径）
- 影响范围: UFT 策略在 `on_init` 阶段（引擎尚未注入交易适配器）读取持仓/撤单是常见写法，属真实崩溃面
- 备注: 与 BUG-N03-005 同一类但不同文件，独立立案便于分别修复；建议 `_trader = NULL` + 入口判空


- 迭代02复核: TEST。DISABLED 测试直接构造 context 后从未 setTrader，另一个 adapter 也未 init；runner 正常会先查找并绑定 trader，再把 context 加入 engine。没有公开契约要求未绑定交易通道的 context 可执行 trader 查询/撤单。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-P06-005

- 状态: 已撤回（接口文档明确暂未实现切片；未证明当前产品承诺支持 bars/ticks）
- 严重级别: Major
- 模块/文件: `src/WtUftCore/WtUftEngine.cpp:179-183`（`get_tick_slice`）、`209-243`（`get_kline_slice`）
- 相关测试: `p06_uft_engine.data_mgr_delegation_and_price`（启用，断言恒为 NULL）
- 输入/前置条件: 任意 `get_kline_slice(sid, code, period, count, times, etime)` 调用
- 期望行为: 写入 `_bar_sub_map[合约-周期-倍数]` 订阅表并向数据管理器取切片
- 实际行为: 函数第一行 `return NULL;`，其后 30 余行（含订阅登记、`KP_Minute5/KP_Minute1/KP_DAY` 周期换算）永久不可达；连带 `on_bar`（`584-603`）的订阅表永远是空表，K 线回调不可能触发
- 复现步骤: `p06_uft_engine.bar_and_l2_push_dispatch` 里 `on_bar` 分发的空表分支即为此证据
- 影响范围: UFT 策略的 `on_bar` 回调链路整体失效；`WtUftEngine.cpp` 约 40 行无法被任何测试覆盖
- 备注: **阻塞覆盖率目标**（死代码），需产品侧确认是「有意关闭 K 线」还是遗留调试语句；本波未改源码

## BUG-P06-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/WtUftTicker.cpp:54-61`（`init`）、`68-166`（`on_tick`）
- 相关测试: `p06_uft_ticker.init_and_forward_before_thread_start`（启用，只覆盖 `_thrd == NULL` 转发支）、`p06_uft_ticker.null_engine_guard_on_forward`
- 输入/前置条件: `WtUftRtTicker t(nullptr); t.init("ALLDAY");` 或 `init()` 时基础数据管理器取不到时段（`_s_info == NULL`）后再走 `_thrd != NULL` 的行情分支
- 期望行为: `init` 与 `on_tick` 对 `_engine`、`_s_info` 一致判空；取不到时段时应拒绝初始化或降级
- 实际行为: `init` 不判 `_engine`（`on_tick` 判）；`_s_info` 在 `on_tick` 的 `timeToMinutes/minuteToTime/isLastOfSection` 与 `run()` 线程体里均无判空；`on_tick` 里 `if(_engine)` 分支内又无条件调用 `_s_info->...`（第 99-109 行在判空之前）
- 复现步骤: 该分支只能由 `run()` 起线程后进入，本波按禁令不调 `run()`，故以源码审阅定级
- 影响范围: 交易时段配置缺失（session id 写错）时 UFT 引擎启动后首个 tick 即崩
- 备注: 属排除清单（需要 `run()` 线程）；修复方向是 `init` 里 `_s_info == NULL` 直接报错返回
## 独立逐项复核（2026-10-29）

本节依据源码、公开接口注释、仓库调用顺序及关联测试定义做静态审查；本次没有运行测试、编译或启动线程复现。DISABLED 用例只说明隔离的输入形状，不作为本次动态复现证据。

### BUG-P06-001：构造后未初始化的管理器指针可造成未定义行为；生命周期和空指针初始化参数需区分

构造函数只初始化 _tm_ticker/_cfg/_notifier（WtUftEngine.cpp:35-51），而 _base_data_mgr、_data_mgr 未初始化；查询函数直接用它们（:118-123,137-142,192-194,265-269）。因此在 init 前调用这些查询确有未定义行为。公开类提供显式 init，常规装配应先初始化；DISABLED 用例正是绕过该生命周期（p06_uft_engine_test.cpp:423-436），本次未运行，故不能据此声称正常启动路径触发。_dependent 当前没有读点（源码检索仅见头文件声明），不能把它列为已证明的运行故障。建议构造时将指针置空并在查询入口返回安全值，或明确并检查必须先 init 的契约。

另一个相邻但不同的问题是 init 接受 dataMgr==NULL，而 UftLatencyTool::initEngine 实际传入 NULL（src/WtLatencyUFT/UftLatencyTool.cpp:276-280）；get_cur_price/get_last_tick 仍无判空地调用 _data_mgr。这不是“构造后未 init”的证据，是否在该工具的有效策略路径调用价格查询尚需调用点/运行证据，建议单独确认，不并入本条已述结论。

### BUG-P06-002：缺分隔符输入会越界取段；所给输入违反标准代码格式

get_commodity_info/get_contract_info/get_session_info(isCode=true) 将 StrUtil::split 结果直接按 ay[0]/ay[1] 读取（WtUftEngine.cpp:118-123,137-142,152-162）；无点字符串只有一个段时，ay[1] 越界。接口注释和 IUftStraCtx.h:127-141,165 的标准代码示例均要求含交易所与合约的点分格式，因此这属于格式非法输入的拒绝/防御性缺陷，不是对合法标准代码路径的证据。DISABLED 用例覆盖此输入定义，但本次未运行（p06_uft_engine_test.cpp:423-445）。建议在拆段后检查结构和非空段，格式错误时返回 NULL；与 BUG-N03-007 属同一输入类别、不同调用点，可共享解析校验但分别验收。

### BUG-P06-003：空 commInfo 解引用源码成立；完整 WTSBaseDataMgr 装配不会产生该测试对象

get_session_info(code,true) 在找到合约后直接对 cInfo->getCommInfo() 的返回值调用 getSessionInfo（WtUftEngine.cpp:158-167）。WTSContractInfo 默认将 m_commInfo 置空且 getter 返回裸指针（WTSContractInfo.hpp:317,351），所以借助自定义/部分装载的 IBaseDataMgr 可以形成该条件。当前测试用 WTSContractInfo::create() 注册未关联品种的对象（p06_uft_engine_test.cpp:448-451），但常规 WTSBaseDataMgr::loadContracts 遇到商品不存在会跳过该合约（WTSBaseDataMgr.cpp:463-467），正式构造合约后在登记前设置 commInfo（:469-485）。因此不能把假对象当作完整生产装载路径的复现；代码层仍有条件空指针风险。启用测试 get_commodity_info 对同类未关联合约会返回 NULL（p06_uft_engine_test.cpp:154-155），显示查询接口已有可空处理先例。建议此处也判空返回 NULL，并将测试定位为部分/替代数据管理器契约。

### BUG-P06-004：未调用 setTrader 的上下文存在未定义行为；第二段适配器场景不能证明 _trader 缺陷

构造函数不初始化 _trader（UftStraContext.cpp:43-50），而交易/仓位入口无判空直接使用（:794-843,865-925）。在 setTrader 前调用时是野指针行为；但 setTrader 是显式装配步骤，当前 UftLatencyTool::initStrategies 在把 context 加入引擎前完成注入（UftLatencyTool.cpp:262-272），引擎随后才在 run() 调 on_init（WtUftEngine.cpp:326-350）。所以确认的是未满足装配前置条件时的不安全状态，正常样例调用顺序未显示该故障。DISABLED 用例第 (1) 段刻意违反该顺序；第 (2) 段已设置 _trader，调用的 getPosition/getUndoneQty 对空映射返回 0（TraderAdapter.cpp:258-282,304-309），并不触及 _stat_map，不能支持该段注释所称“fresh adapter 空表崩溃”，也没有隔离验证 N03-005。建议将 _trader 初始化为 NULL，未装配时返回安全值/错误，并明确 setTrader 前置条件。

### BUG-P06-005：当前接口契约明确暂未实现；死代码现象确认，但不构成当前行为缺陷

IUftStraCtx.h:169-182 将 K 线与 tick 切片标注为“暂未实现”；WtUftEngine.cpp:176-177,207-212 也注明直接返回 NULL，启用测试断言该返回值（p06_uft_engine_test.cpp:225-236）。因此 get_kline_slice 后续订阅/周期代码不可达及 _bar_sub_map 无法由该方法写入是源码事实，但不能据此把“应返回非空/on_bar 必须由 K 线 API 驱动”认定为当前承诺。该项与 BUG-R173-001、BUG-R123-001、BUG-R194-001 重复登记。建议产品先明确 UFT 是否承诺支持 bars/ticks；若仍暂未实现，删去死代码或明确 stub；若要求实现，再按接口规格实现与验收，不作为覆盖率缺口强行启用失败断言。

### BUG-P06-006：缺失交易时段会在初始化后的运行路径空指针解引用；线程路径未动态验证，且已有重复记录

WtUftRtTicker::init 无条件通过 _engine 查 session 并把结果存进 _s_info（WtUftTicker.cpp:54-61）；WtUftEngine::run 随后立即调用 ticker run()（WtUftEngine.cpp:326-350），ticker 的 run 读 _s_info->id()（WtUftTicker.cpp:172-183），活动线程的 on_tick 也在判 _engine 前使用 _s_info（:95-110）。因此 session id 未装载/配置错误时，正常启动过程存在源码可确认的空指针风险。本轮启用测试只证明 getSession 返回 NULL 时 init 本身不访问 _s_info、未启动线程的 on_tick 走安全早退（p06_uft_engine_test.cpp:371-420）；DISABLED/排除说明没有运行 run() 线程，本次也未复现。传 nullptr engine 后再调 init 是违反构造依赖的另一个边界，不应与“合法 engine 找不到 session”混为一谈。建议使 ticker 初始化可报告失败，并在启动线程/处理 tick 前检查 session；拒绝无效 session 而不是继续运行。该问题已在 R78 前账中指向 BUG-R11-005/BUG-P06-006，同类重复记录不应再次计数。
