# R15 WtCore 缺陷登记（lane R15，只登记不改产品代码）

范围：`HftStraBaseCtx.h` `HftStraContext.cpp` `SelStraBaseCtx.cpp` `WtExecuterFactory.cpp` `CtaStrategyMgr.cpp` `HftStrategyMgr.cpp` `ParserAdapter.cpp` `WtLocalExecuter.cpp`。
分级：P0 崩溃/进程终止 · P1 功能错误 · P2 健壮性/资源 · P3 一致性/可读性 · 需接缝（可达性阻塞，非缺陷但挡覆盖率）。
所有行号取自本 lane 实读的源码；未修改任何产品代码、未放宽任何断言。

---

## BUG-R15-001 【P1】HftStraBaseCtx::setUserTag 写入零容量 circular_buffer 被静默忽略，订单 userTag 链条失效
- 位置：`src/WtCore/HftStraBaseCtx.h` L774（`boost::circular_buffer<OrderTag> _orders;` 默认构造，**全工程没有任何 `set_capacity` 调用**，构造/`on_init`/`init_outputs` 均不填容量）、L646-649（`setUserTag`：`_orders.push_back({ localid, usertag });`）。
- 触发条件：任何一次带标签的下单回报登记 —— `stra_buy/stra_sell`（`HftStraBaseCtx.cpp` L234-236、L255-257、L298-300、L318-320 的 `for (auto localid : ids) setUserTag(localid, userTag);`）。当前 Boost `circular_buffer` 对 capacity 为 0 的 `push_back` 直接不插入；不会因该调用 assert 或触发未定义行为。
- 连带后果：当前 `_orders` 始终为空，`getOrderTag`（L625-637）返回 `""`，`HftStraContext::on_trade/on_order/on_entrust` 转发给策略的 userTag 恒为空串 —— 策略侧「按标签跟踪订单」功能不可用。
- 测试侧处置：`R15CtxTest.order_tag_default_capacity_preserves_registered_tag` 已启用并按正确标签语义断言，定向运行 FAIL（实际返回空串）。完整标签链 `R15CtxTest.DISABLED_order_tag_set_get_erase_with_initialized_capacity` 和缺键 `R15CtxTest.DISABLED_order_tag_lookup_miss_does_not_match_or_erase_neighbor` 仍禁用；非零容量场景缺少安全测试接缝。
- 期望：初始化 `_orders` 容量（并明确容量上限/淘汰策略），或改用按 localid 精确索引的容器；不可把“容量为零时静默丢弃”当作可接受的策略。
- 状态: 已复现（本地定向用例因默认容量下标签未保存而 FAIL；生产影响范围待验证）

## BUG-R15-002 【待验证】getOrderTag/eraseOrderTag 的 lower_bound 结果不校验键相等 ⇒ 容量启用后查询失配返回后继标签、删除失配抹掉后继
- 位置：`src/WtCore/HftStraBaseCtx.h` L625-637（`getOrderTag`：`lower_bound` 后只判 `it == _orders.end()`，没有 `it->_localid == localid`）、L656-668（`eraseOrderTag` 同形，失配时直接 `_orders.erase(it)`）。
- 触发条件：如果 `_orders` 已能保存标签且包含 (1,AAA)、(5,EEE)，则 `getOrderTag(3)` 会返回 `"EEE"`，`eraseOrderTag(2)` 会删掉 5 号标签。按现有零容量实现，`_orders` 始终为空，这个 lower_bound 错误目前被 BUG-R15-001 遮蔽，并非当前可观察路径。
- 测试侧处置：旧用例曾将 `getOrderTag(3)=="EEE"` 等错误行为写成预期；现已改成精确键语义，当前名称为 `DISABLED_order_tag_lookup_miss_does_not_match_or_erase_neighbor`。当前默认容量下标签插入不会成功，仍须保持禁用；先启用可保存的标签容器或提供确定性接缝后才能验证邻项查找和删除。
- 期望：命中后校验 `it->_localid == localid`，失配返回 `""` / 不删除；在修复 R15-001 的同一变更中覆盖容量开启后的缺键行为。
- 迭代02第二轮前状态（历史）: - 状态: 待验证（缺键算法错误成立，但当前零容量使其不可观察）
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）

## BUG-R15-003 【P2】SelStraBaseCtx 的 load_data/load_userdata 对 RapidJSON 字段校验不足，缺字段/类型不符可触发断言或错误读取
- 位置：`src/WtCore/SelStraBaseCtx.cpp`
  - `load_userdata` L198-227：RapidJSON object 成员名 `m.name` 按对象合同为字符串，原登记对 `m.name.GetString()` 的担忧不成立；`m.value.GetString()` 没有 `IsString()` 检查，非字符串值可能触发 RapidJSON assertion。
  - `load_data` L229-376：fund 段 L250-258 无条件 `jFund["total_profit"].GetDouble()`、`jFund["tdate"].GetUint()`；仓位段 L265-300 无条件 `pItem["code"].GetString()`、`["closeprofit"]`、`["lastentertime"].GetUint64()` 等 8 个字段；明细段 L305-334 无条件取 9 个字段（只有 `maxprice/minprice/opentdate` 有 HasMember 守卫，见 L313-324 —— 说明作者知道要防，但只防了三处）。
- 触发条件：`load_data` 直接读取 `root["positions"]`，并对多处必需字段调用 `GetString/GetDouble/GetUint/GetUint64`；`load_userdata` 对值直接 `GetString`。不完整或类型错误的 JSON 可能触发 RapidJSON assertion。旧版本文件是否允许缺字段需以持久化格式合同确认，不能把所有旧文件形态一概判为非法。
- 测试侧处置：`r15_sel_persist.cpp` 的手拼 JSON 带齐字段；`broken_json_and_empty_file_are_ignored` 只覆盖解析失败/空文件/文件缺失早退，不覆盖解析成功但缺字段或类型错误的 JSON。本轮未运行测试。
- 期望：按字段合同先验证 `HasMember`/`IsObject`/`IsString` 等，再取值；明确可选字段默认值与必需字段错误策略，记录日志并拒绝或跳过相应数据段。
- 状态: 待验证（不完整/错误类型输入会触发不安全取值；持久化 schema 合同待确认）

## BUG-R15-004 【P2】WtLocalExecuter::writeLog 用 strcat 拼固定 2048 thread_local 缓冲，无长度上限（与 BUG-R10-010 同类的独立位置）
- 位置：`src/WtCore/WtLocalExecuter.cpp` L400-409：`static thread_local char szBuf[2048] = {0}`；`fmtutil::format_to(szBuf, "[{}]", _name)` 之后 `strcat(szBuf, message)`。
- 触发条件：`message` 长度不受调用方约束（ExecuteUnit 撮合日志、错误回报文案），`[name] + message > 2047` 即越界写 thread_local 数组。对照：`WtArbiExecuter.cpp` L352-358 已登记为 BUG-R10-010；这是另一个函数位置的同类风险，不是同一缺陷位置的重复项。无容量参数的 `format_to` 也不能据此认定为有界。
- 测试侧处置：启用用例只喂短消息（`r15_mgr_executers.cpp` 的 `local_executer_null_init_and_empty_channel_events`），越界形态不构造。
- 期望：使用动态字符串，或调用明确接受目标容量的格式化 API 并定义截断行为。
- 状态: 待验证（静态越界链成立，长消息用例未运行）

## BUG-R15-005 【P2】SelStraBaseCtx::on_tick 信号触发段解引用 get_session_info 的结果且无判空
- 位置：`src/WtCore/SelStraBaseCtx.cpp` L562-571：
  ```cpp
  WTSSessionInfo* sInfo = _engine->get_session_info(stdCode, true);
  if (sInfo->isInTradingTime(_engine->get_raw_time(), true))
  ```
  而 `WtEngine::get_session_info`（`WtEngine.cpp` L104-112）在 commodity 的 session id 查不到时可以返回 NULL —— 同函数 L127-129（`WtEngine::append_signal` 链）也是同样的裸解引用形态。
- 触发条件：任何「先有仓/信号、后改基础数据表把时段删掉/打错」的运维形态；真实 `WtBaseDataMgr`（键不存在返回 NULL，不像本 lane 替身那样兜底取首个会话）下就是启动崩溃。
- 测试侧处置：本 lane 替身 `q01sel::BaseDataMgr::getSession` 对未知 sid 兜底返回首个会话，因此启用用例里 `sInfo` 恒非 NULL，掩盖不了也证明不了该分支；NULL 形态未构造用例（会崩宿主）。
- 期望：`sInfo == NULL` 时记录原因并跳过本次信号触发，保留 pending signal/仓位状态并继续 `on_tick` 后续清理逻辑；不要不加分析地从整个回调提前返回。
- 迭代02第二轮前状态（历史）: - 状态: 待验证（真实数据管理器在 session 缺失时返回 NULL；负例测试未运行）
- 状态: 已合并（迭代02：主编号 BUG-P01-001）

## BUG-R15-006 【需接缝】R15 lane 的无线程约束留下 HFT Engine run 标记落地覆盖缺口
- 位置：
  - `src/WtCore/WtCtaTicker.cpp` L37/L122/L196/L202/L250/L271（`run/stop/cleanup/task_loop` 的 `TimeUtils::getDateTime`、`StdUniqueLock`、`sleep_for`）；
  - `src/WtCore/WtHftTicker.cpp` L47/L114/L177/L183/L228/L246（同形）；
  - `src/WtCore/WtSelTicker.cpp` L47/L117/L180/L186/L226（同形）；
  - `src/WtCore/WtHftEngine.cpp` L56/L61（release/init 转调 WtEngine）、L81-110（`run()` 里 marker.json 组包落地；需先提供有效 cfg、data manager 和 adapter manager，`WtEngine` 构造时 `_adapter_mgr` 初始化为 NULL，须通过公开 `set_adapter_mgr()` 设置）。
- 现象：ticker 的 `on_tick/on_bar` 入队口可在主线程调用，但 R15 lane 禁止起线程/挂钟，因而本 lane 不验证循环消费。此约束不等于仓库没有 ticker run 测试：其它覆盖目录已有 CTA/HFT/Sel ticker 的运行相关用例。WtHftEngine 的 marker 落地段仍需单独核对是否存在直接验收。
- 估计留白行数：原列三份 Ticker 的 17 行和循环周边约 55 行是本 lane 当前未验证范围，不应表述为全仓整体不可达；WtHftEngine marker 段约 45 行仍是本条重点缺口。
- 建议接缝：优先复用现有 ticker 驱动方式或抽取纯 marker 序列化逻辑；只有在现有测试无法提供确定性观测时，才考虑最小测试 seam，不增加泛化生产 API。
- 状态: 已撤回
- 撤回说明：这是测试覆盖缺口，不是已发现的产品缺陷；待验事项保留在本记录中。

---

## 2026-10-29 逐项复核

本节的初次静态复核基于源码、接口和测试源文件，未运行测试；其后定向运行结果分别记在对应条目。分类区分产品行为、条件性输入路径和 lane 测试范围，不把测试文件存在当作通过证据。

### BUG-R15-001 — 当前功能缺陷成立；“assert/UB”属于误报

- 源码与容器行为：`HftStraBaseCtx::_orders` 默认构造且没有容量设置；`setUserTag()` 无条件 `push_back`，随后 `getOrderTag()` 通过该容器查找。可见 Homebrew Boost 1.88 的 `boost/circular_buffer/base.hpp::push_back_impl` 对零容量 push 直接不保存元素，因此实际结果是标签被静默丢弃，没有证据支持原登记中的 assert-abort/UB。
- 接口与下游：下单入口把 localid/tag 交给 `setUserTag()`；`HftStraContext` 的委托、成交等回报再通过 `getOrderTag()` 转发。因标签从未留存，当前 API 可调用但标签功能不工作。
- 测试前提及运行证据：已启用 `R15CtxTest.order_tag_default_capacity_preserves_registered_tag`，通过公开的 `setUserTag(1, "AAA")` 登记再查询，定向运行 FAIL：实际返回空串。需要预置非零容量的完整链和缺键用例仍禁用：容器成员为 private，现有测试没有安全设置容量的接缝。
- 修复方向：决定标签保留容量及淘汰策略后初始化容器，或改用 localid 精确索引；补充正常登记、重复 localid、容量边界与删除测试。测试源 `r15_hft_stra_context.cpp` 中关于 BOOST_CB_ASSERT/UB 的注释和原登记一样不符合当前可见 Boost 实现；当前启用回归与仍禁用的容量用例状态见本条测试侧处置。
- 证据边界：默认容量与写入路径由源码确认，本地编译及定向运行观察到标签被丢弃；不据此推断所有平台的 Boost 版本或所有生产策略都依赖 userTag。
- 状态: 本地定向测试复现，生产影响面待验证

### BUG-R15-002 — 缺键查找/删除错误成立，但当前受 R15-001 遮蔽

- 源码与接口：`getOrderTag()` 和 `eraseOrderTag()` 在 `lower_bound` 后未比较 `_localid`，因此一旦容器里存在大于请求 id 的元素，查找会返回后继标签，删除会移除后继项。按 localid 查询/删除接口语义应仅作用于精确 id。
- 测试前提：当前 capacity=0 时插入是 no-op，禁用用例不能构造出有序元素，也就没有实际执行到邻项错误；原 `getOrderTag(3)=="EEE"` 错误断言已改为缺失应返回空串且删除缺失 ID 不影响邻项。由于容器是 private 且没有安全设置容量的接缝，该回归仍禁用、NOT_RUN。
- 修复方向：lower_bound 后先比较键，只有相等才读/删；与 R15-001 容量修复一起添加容量启用的缺键测试。
- 证据边界：错误条件由算法静态可推导；在当前默认状态不可观察，且本轮未运行重构后的用例，因此保留待验证，不将其标为已合并或已撤回。
- 状态: 待验证（条件性缺陷，当前实现路径被零容量遮蔽）

### BUG-R15-003 — 不完整/错类型持久化输入的条件性健壮性缺陷

- 源码与接口：`load_data()` 读取 `positions` 及 fund/position/detail 的多个必需字段后直接 `GetXxx()`；`load_userdata()` 对 `m.value` 直接 `GetString()`。RapidJSON 对缺成员或类型不匹配的取值可能触发 assertion。相反，`m.name` 是 JSON object 成员名，按 RapidJSON 对象合同本来就是字符串；原登记把它也列作无 `IsString()` 的错误取值是不成立的。
- 测试前提：`r15_sel_persist.cpp` 手拼的是字段齐全的 JSON；`broken_json_and_empty_file_are_ignored` 覆盖解析失败、空文件/缺文件的早退，不覆盖语法正确但缺字段或值类型错误。未见可证明旧 schema 缺字段应被接受的兼容性合同。
- 修复方向：先验证 root/section 对象和必需字段类型，再按持久化版本合同选择拒绝整份文件、跳过单段或使用明确默认值；对 `load_userdata` 的 value 类型作显式处理。
- 证据边界：不安全取值链静态成立；具体旧版本 JSON 是否属于合法输入、是否在启动中断言，仍需 schema 合同与定向负例确认；没有运行测试。
- 状态: 待验证

### BUG-R15-004 — 固定缓冲区越界风险成立，与 R10 同类但位置不同

- 源码与接口：`WtLocalExecuter::writeLog()` 将格式化的名称写入 2048 字节 thread-local 数组后无界 `strcat(message)`；调用接口没有可见的消息长度上限。足够长的消息会写越缓冲区。与 R10-010 是不同函数/位置，应分别保留，但修复模式可统一。
- 测试前提：`r15_mgr_executers.cpp` 只检查空初始化/空通道事件和短日志路径；没有长消息边界用例，本轮未运行测试。
- 修复方向：构造 `std::string` 后记录，或用显式容量的格式化函数并定义截断/错误处理；不能把无容量 `fmtutil::format_to` 的单步写法称为安全截断。
- 证据边界：越界条件是根据容量与无界拼接静态确认；未运行长消息复现，也未评估日志下游最大长度合同。
- 状态: 待验证

### BUG-R15-005 — session 缺失时信号触发段空指针，条件路径成立

- 源码与接口：`SelStraBaseCtx::on_tick()` 先按待触发信号查 `_engine->get_session_info(stdCode, true)`，随后无判空调用 `isInTradingTime()`；引擎查不到商品/session 时返回 NULL。`IBaseDataMgr::getSession()` 的指针返回也允许查找失败，真实基础数据管理器按未知 session 返回 NULL。
- 测试前提：R15 的 `q01sel::BaseDataMgr::getSession` 对未知 id 回退首个会话，因而掩盖该分支；没有缺 session 的定向用例。该触发要求存在待处理信号并且基础数据 session 缺失，不是所有行情回调都会触发。
- 修复方向：NULL 时记录缺失 session，保留信号并跳过本次触发，然后继续更新浮盈、策略 tick 和 userdata 保存；另对 append_signal 的相似路径独立审视。
- 证据边界：空指针调用链与真实 manager 的空结果可静态核实；缺 session 配置的生产合法性以及策略决策仍需配置合同确认；本轮未运行。
- 状态: 待验证（条件触发下的源码缺陷）

### BUG-R15-006 — lane 测试边界项，原“全仓不可达”判断过宽

- 现有覆盖：`src/TestUnits/coverage/q01_wtcore3/r170_ticker_run_test.cpp` 和 `src/TestUnits/coverage/n02_wtcore_engines/r160_hft_ticker_test.cpp` 的测试源调用对应 ticker `run()` 并驱动分钟闭合；`src/TestUnits/coverage/q01_wtcore3/r192_wtcore_ticker_residual_test.cpp` 覆盖 CTA/Sel/HFT ticker 多个剩余时间分支；`src/TestUnits/coverage/q01_wtcore3/r181_wtcore_rest_test.cpp` 还有 ticker 生命周期路径。Sel/CTA engine marker `run()` 也有 `src/TestUnits/coverage/n02_wtcore_engines/r120_sel_engine_run_test.cpp`、`src/TestUnits/coverage/q01_wtcore3/r90_cta_engine_gap_test.cpp` 等测试源。
- 剩余缺口：当前 R15 lane 的无线程/挂钟约束确实不允许在本轮运行这些线程测试；测试源存在不代表它们已运行或通过。静态检索未找到直接覆盖 `WtHftEngine::run()` marker 内容的同类用例，因此应把该 marker 段作为剩余待验范围，不把三个 ticker 全部记为全仓不可达。
- 接口与建议：ticker 的 `run()` 是公开调用入口，仓库已有测试使用该入口；不应只为 R15 增加公开 queue peek/drain API。若 HFT engine marker 需要确定性验证，可通过现有初始化接口满足 cfg/data manager/adapter manager 前提，或抽取纯序列化函数；先复用现有夹具，避免扩大生产接口。
- 状态: 已撤回（测试范围差额；不是已证实产品缺陷，测试未运行）

## 复核前人与规则口径（规则：引用前先自验）
- R10 标注的六个「需要接缝」面（EventNotifier ~340、HftStraBaseCtx 报单链 ~260、WtDtMgr reader ~230、执行单元工厂 ~210、Sel run/init ~75、线程池 ~70）本 lane **一律未硬闯**；`WtExecuterFactory::loadFactories` 只打目录扫描/`free_library` 分支，单元主体仍被 BUG-R10-002 挡着。
- `CodeHelper` 合约键形：本轮全部用三段式分月合约（`SHFE.ag.2512`、`CFFEX.no.2512`），与 `CodeHelper.hpp:601-616` 规则一致。
- BUG-R15-001 当前确认的是标签被静默丢弃；BUG-R15-002 是容量修复后需要一并消除的条件性精确键错误。不能把当前表现描述成“全链路不安全”或进程崩溃；修复与验收需覆盖标签登记、回报转发和精确删除。
