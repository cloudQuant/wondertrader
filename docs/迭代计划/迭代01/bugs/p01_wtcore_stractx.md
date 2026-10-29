# 缺陷记录复核 — p01_wtcore_stractx（WtCore CTA 策略上下文）

> 复核范围：对照 `CtaStraBaseCtx`、`WtEngine`、`ICtaStraCtx`、持久化写入逻辑及本 lane 测试源码。`DISABLED_` 用例未执行；裸引擎/测试夹具不自动等同于生产合法生命周期。

## BUG-P01-001 — CTA 平仓与待处理信号路径未检查 session

- 状态: 静态空指针路径确认；生产可达性及动态证据不足
- 严重级别: Major（基础数据缺 session 时）
- 模块/文件: `src/WtCore/CtaStraBaseCtx.cpp` `stra_exit_long`、`on_tick`
- 源码事实: `stra_exit_long` 先检查 `commInfo`，但对 `commInfo->getSessionInfo()` 的结果直接调用 `offsetTime/getCloseTime`。`on_tick` 在 `_sig_map` 命中时调用 `_engine->get_session_info(stdCode, true)` 后直接调用 `isInTradingTime`；`WtEngine::get_session_info` 可在品种或 session 未解析时返回 `NULL`。
- 测试事实: `DISABLED_P01CtaCrashClass.unchecked_null_pointers_segfault` 构造了一个未给 commodity 绑定 session 的 CZCE 品种，并首先调用 `stra_exit_long`，与第一条静态空指针路径相符，但测试禁用、未动态执行。这个调用如果终止，后续历史数据调用不可达。测试没有建立 `_sig_map` 后触发 tick 的输入，故 `on_tick` 的空 session 路径目前只有源码证据。
- 影响边界: 对缺失/未绑定时段的品种有直接解引用风险。加载的生产基础数据是否总能满足两种 session 获取方式的约束，当前未提供运行证据；不得据此宣称常规实盘链路已崩溃。
- 修复方向: 对两处 session 结果判空并给出可诊断日志；补充启用态、分别覆盖平仓和待处理信号的边界测试，并验证标准数据装载路径。

## BUG-P01-002 — 未初始化 WtEngine 下历史数据接口间接解引用空/未初始化管理器

- 迭代02第二轮前状态（历史）: - 状态: 生命周期边界候选；接口契约不足，动态证据无效
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: 暂不定级
- 模块/文件: `src/WtCore/CtaStraBaseCtx.cpp` `stra_get_bars/stra_get_ticks/stra_get_last_tick`；`src/WtCore/WtEngine.cpp` `get_kline_slice/get_tick_slice/get_last_tick`
- 源码事实: Context 并不直接访问 `_data_mgr`。`stra_get_ticks/stra_get_last_tick` 转调 Engine 后，Engine 分别直接使用 `_data_mgr->get_tick_slice` 与 `_data_mgr->grab_last_tick`；`stra_get_bars` 转调 `get_kline_slice`，该函数先访问 `_base_data_mgr`，命中品种后再访问 `_data_mgr`。相关路径没有空指针检查。
- 测试事实: 测试夹具 `P01CtaEngine::attach` 只设置 `_base_data_mgr/_hot_mgr`；它不调用 `WtEngine::init`，而基类构造函数未初始化 `_data_mgr`。因此测试注释中“`_data_mgr` 恒为 NULL”不成立：该成员在此夹具中是未初始化指针。禁用用例首先执行 `stra_exit_long`，之后的历史数据调用也不是可执行证据；未运行。
- 契约判断: `ICtaStraCtx` 说明这些方法返回历史数据片段，但没有承诺在 Engine 尚未 init 或 data manager 缺失时返回空指针。正常初始化流程会把管理器指针传给 `WtEngine::init`；当前证据没有证明未初始化上下文属于支持用法，或生产调用传入空 data manager。
- 结论: 静态上可确认违反初始化前置条件时会走到无保护解引用；不能据此定为当前正常生命周期中的 Blocker。优先澄清 `WtEngine::init` 对依赖是否强制非空。
- 修复方向: 若 null manager 是支持状态，Engine 的三个数据接口应返回 `NULL` 并记录诊断；若不支持，应在初始化边界校验依赖并明确生命周期契约，不把裸引擎测试当作生产复现。

## BUG-P01-003 — stra_set_position 对无效品种/代码缺少前置校验

- 状态: 候选问题；输入契约和运行结果尚未验证
- 严重级别: Minor（具体影响取决于无效代码类型）
- 模块/文件: `src/WtCore/CtaStraBaseCtx.cpp` `stra_set_position`、`append_signal`、`do_set_position`
- 源码事实: `stra_set_position` 先 `sub_tick`，再调用 `append_signal`；`append_signal` 写入 `_sig_map` 并保存状态。`stra_enter_long/short` 在发信号前会检查 `get_commodity_info`，而 `stra_set_position` 没有该品种级检查。后续 `do_set_position` 也会在 `get_commodity_info` 为空时直接返回。
- 需要收窄的原主张: 现有 `get_commodity_info` 是按标准代码解析到 exchange/product 后查 commodity，不是精确合约存在性检查；所以原例 `SHFE.rb9999` 不能证明“已知品种下不存在的合约会被拒绝”，`stra_enter_long/short` 也不保证精确合约校验。只有 commodity 不存在的输入才能支持与入口商品校验的不一致结论。
- 影响边界: 对确实无 commodity 的代码，源码显示信号可先写入并持久化，后续 `do_set_position` 因 commodity 为空返回；若收到同代码 tick，还可能先触发 BUG-P01-001 中未检查 session 的路径。仓库没有对应启用测试或运行记录，不能声称实测形成僵尸信号或必然成交失败。
- 修复方向: 先约定该 API 对无效标准代码的行为；若应拒绝，在 `sub_tick/append_signal` 前做与 `stra_enter_*` 一致的 commodity 检查，并为不存在的 commodity 增加持久化状态断言。若要求精确合约校验，应另行明确并实现合同级检查。

## BUG-P01-004 — load_data 对必需 JSON 字段缺少 schema 校验

- 状态: 静态健壮性问题确认；兼容性缺陷未证实
- 严重级别: Minor（仅旧版/人工修改/不完整状态文件）
- 模块/文件: `src/WtCore/CtaStraBaseCtx.cpp` `load_data`
- 源码事实: parse 通过后，`positions` 与 `conditions` 直接用 `root["..."]` 读取而不先检查 `HasMember`；位置明细、条件条目里的若干字段也直接取值。与此同时，当前 `save_data` 总会写入 `positions`、`conditions` 及其标准子字段；`fund/signals/utils` 对部分可选块已有 `HasMember` 检查。
- 测试事实: lane 的持久化往返测试使用完整的规范 JSON；没有 `{}`、缺少 `positions/conditions` 或缺少条目字段的启用态夹具。当前没有证据证明受支持的历史文件版本会省略这些字段，也没有实际触发 RapidJSON 缺成员行为的运行记录。
- 结论: 对非规范文件缺少 schema 校验属实；将其说成旧版数据必然触发断言或“读到垃圾值”超过现有证据。是否属于兼容性缺陷取决于是否承诺读取旧 schema 或人工编辑文件。
- 修复方向: 若 loader 需容忍历史/部分状态文件，应验证 root 类型、必需成员、类型及数组/对象形状，失败时保留/拒绝文件并报告具体字段；若只支持 `save_data` 产出的版本，记录版本和 schema 前置条件。补齐最小缺字段夹具后再验证 Debug/Release 行为。
