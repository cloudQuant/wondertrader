# p02_wtcore_engines2 缺陷登记（2026-10-29 逐项复核；未改生产代码或测试）

模板见 `docs/迭代计划/迭代01/bugs/README.md`。

---

## BUG-P02-001 通道就绪后 buy/sell/cancel 直接解引用空 _trader（三个 Executer 同构）

- 迭代02第二轮前状态（历史）: - 状态：待验证（源码缺少 `_trader` 守卫；ready-without-trader 是否符合产品生命周期尚未确认）
- 状态： 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别：崩溃（SIGSEGV）/ 缺陷类，影响 3 个执行器
- 位置：
  - `src/WtCore/WtLocalExecuter.cpp:328-392`（`buy` / `sell` / `cancel(uint32_t)` / `cancel(code,isBuy,qty)`）、`WtLocalExecuter.cpp:798-824`（`on_channel_ready`）
  - `src/WtCore/WtDiffExecuter.cpp:379-432` + `on_channel_ready`
  - `src/WtCore/WtArbiExecuter.cpp`（同一模式的四个下单入口）
- 静态路径：直接手工调用 `on_channel_ready()` 后再调买卖撤单，代码确会越过 `_channel_ready` 检查并访问 `_trader`。但 `TraderAdapter` 的就绪通知只发给登记 sinks；本轮未找到 Runner 在没有 trader 时向执行器送 ready 的产品路径。`setTrader(NULL)` 若在已 ready 后发生也会保留原 ready 位，但未找到产品生命周期中会这样解绑的调用。
  同类入口还有 `sell` / `cancel(localid)` / `cancel(stdCode,isBuy,qty)`；`WtDiffExecuter`、`WtArbiExecuter` 三份实现同构。
  另外 `on_position_changed` / `set_position` 里的风控限单判断写作 `if(_trader && !_trader->checkOrderLimits(...))` —— 说明作者知道 `_trader` 可能为空，下单入口却没有同样的护栏，属同一缺陷类的漏检。
- 期望：`_trader == NULL` 时下单入口返回空 `OrderIDs` / `false`；或 `on_channel_ready` 不在没有交易通道时置位。
- 实际：若人工构造上述 ready-without-trader 序列，会进入未受保护的 `_trader` 调用；产品路径是否可达待验证。
- 处置：启用用例只走 `_channel_ready == false` 护栏。`P02ExecuterCrashClass.DISABLED_ChannelReadyWithoutTraderAndBadPolicyConfigs` 只有注释、测试体为空，并未执行或构造崩溃输入。
- 收益：无（仅登记）。

---

## BUG-P02-002 `getUnit()` 未校验 policy 配置，非法配置直接崩而不是拒绝

- 状态：部分重复；Arbi/Diff 策略链已合并到 BUG-R10-001，Local 同形路径仍待配置合同确认
- 严重级别：崩溃（配置驱动，可被上线误配触发）
- 位置：`WtLocalExecuter.cpp:170-222`、`WtDiffExecuter.cpp:243-286`、`WtArbiExecuter.cpp:163-…`
- 复现（缺陷类三条）：
  1. `init()` 的配置里 **没有 `policy` 段** → `WTSVariant* policy = _config->get("policy")` 返回 NULL → 下一行 `policy->has(commID)` 空指针；
  2. 有 `policy` 但既没有该商品ID也没有 `default` → `cfg = policy->get(des)` 为 NULL → `cfg->getCString("name")` 空指针；
  3. `policy.default.name` 不含 `.`（写成 `"Snel"` 而非 `"Snel.Local"`) 时，`WtExecuterFactory::createExeUnit(name)` 会在拆分段数小于 2 时返回空 `ExecuteUnitPtr`，不是下标越界。**状态：已撤回（“name 无点号导致崩溃”子主张与源码不符）。**
  缺 `policy` 时 `init()` 仍可返回 true；没有商品策略且无 `default` 时 `cfg` 为空。Arbi/Diff 的同一空指针链已登记在 BUG-R10-001；Local 的同形链仍待合同确认。
- 期望：`init()` 阶段拒绝非法配置并返回 `false`（或 `getUnit` 判空后返回空指针，让上层记日志跳过）。
- 实际：缺策略/缺 default 形态为运行期空指针；name 无点号由工厂拒绝创建。
- 处置：禁用测试是空体，不是有效崩溃证据。本轮未执行崩溃输入。

---

## BUG-P02-003 `WtEngine::get_adjusting_flag()` 丢弃数据管理器的返回值

- 状态：产品缺陷（修正现状式断言后，本机定向测试 FAIL）
- 严重级别：逻辑错误（引擎读取到的复权标志恒为 0）
- 位置：`src/WtCore/WtEngine.cpp:771-777`
- 代码：
  ```cpp
  uint32_t WtEngine::get_adjusting_flag()
  {
      if (_data_mgr)
          _data_mgr->get_adjusting_flag();   // 返回值被丢弃
      return 0;
  }
  ```
- 复现：修正后的 P02 测试注入返回 `1` 的 `WtDtMgr` 替身并要求引擎转发；
  `R211SelEngine.AdjustingFlagPropagatesDataManagerValue` 注入返回 `7` 的替身，替身自身和空 manager 返回 0 的对照均未失败，引擎实际返回 0，定向 FAIL。
- 期望：`return _data_mgr ? _data_mgr->get_adjusting_flag() : 0;`
- 实际：当前 accessor 恒返回 0；依赖其非零标志选择复权计算分支的路径可能受影响，实际策略及历史数据影响范围未在此测试中验证。

---

## BUG-P02-004 `WtEngine` 一组未判空的依赖解引用（空指针类）

- 状态：待验证（未判空点可静态定位；依赖是否允许缺失及有效初始化可达性待确认）
- 严重级别：崩溃（初始化顺序或配置缺失时）
- 位置与触发条件（同一个缺陷类，集中登记）：
  - `WtEngine.cpp:779-841` `sub_tick()`：首行 `_hot_mgr->getRuleTag(stdCode)`，`_hot_mgr` 未判空；
    `get_exright_factor()`（`WtEngine.cpp:751-769`）在非股票分支同样直接 `_hot_mgr->getRuleTag(...)`。
  - `WtEngine.cpp:592-600` `get_tick_slice()` / `get_last_tick()`、`WtEngine.cpp:690-749`
    `get_cur_price()` / `get_day_price()`、`WtEngine.cpp:644-669` `handle_push_quote()`：都无条件解引用 `_data_mgr`。
  - `WtEngine.cpp:956-1004` `do_set_position()`：`commInfo = _base_data_mgr->getCommodity(...)` 后不判空即
    `commInfo->calcFee(...)`；合约未在 commodities.json 里登记时（配置漏项）第一笔非零仓位即崩。
  - `load_fees()`（`WtEngine.cpp:843-877`）：`StrUtil::split(fullPid, ".")` 后直接取 `ay[1]`，
    费率文件里出现不含 `.` 的键即越界。
- 期望：这些入口要么在 `init()` 后必然具备依赖（则应显式断言/报错退出），要么判空降级。
- 实际：依赖为空时会进入未受保护的空指针访问；这些依赖在合法初始化中的可空性待验证。
- 处置：本 lane 引擎用例注入 `WtDtMgr` 替身与空基础数据管理器，只走「查不到商品 → 早退」分支。`P02EngineCrashClass.DISABLED_NullHotMgrAndUnseededBaseDataPaths` 只有注释、测试体为空；这些入口未被动态验证。

---

## BUG-P02-005 Local/Arbi `set_position()` 的组合合约结果被算完就丢

- 状态：静态逻辑缺陷成立；实际执行单元派发未动态验证
- 严重级别：逻辑错误（`groups` 配置不生效）
- 位置：`src/WtCore/WtLocalExecuter.cpp:505-592`、`src/WtCore/WtArbiExecuter.cpp:404-540`
- 描述：函数先按 `_groups` 把成员合约轧成 `real_targets[gpInfo->_name] = gpQty`，并从成员上扣减；
  随后第一个派发循环遍历的却是入参 `targets`（不是 `real_targets`），第二段清理循环同样只看 `targets`。
  因此 `real_targets` 的全部计算结果无人使用，`groups`（如 `SHFE.ags: {ag:1, au:2}`）在本地执行器里没有任何效果。
- 期望：派发与差量清算应基于 `real_targets`，或明确删除该计算。
- 实际：两实现都会构造 `real_targets`，后续派发/清理仍以原始 `targets` 查找；组合目标转换没有送到执行单元。
- 处置：`InitParsesScaleClearListAndGroups` 只验证配置解析；`SetAndDeltaPositionWithoutExecutableUnits` 因工厂无插件无法观察派发结果。修复方向是定义 pseudo-code 与成员代码的派发/strict-sync 语义，再让转换后的 map 贯穿派发、差量和清理；本轮未验证交易行为。

---

## BUG-P02-006 `WtDiffExecuter` 的 JSON 读取、单元失败与持久化失败处理不完整

- 状态：混合项；字段类型、缺单元信号丢弃、持久化失败分别按下方复核结论处理
- 严重级别：健壮性 / 数据一致性
- 位置：`src/WtCore/WtDiffExecuter.cpp:113-172`（`load_data`）、`501-545`（`on_position_changed`）
- 描述：
  1. `load_data()` 对数组项直接 `jItem["code"].GetString()` / `jItem["target"].GetDouble()`，
     未 `HasMember`/`IsString` 校验；`execdata/<name>.json` 被人工编辑或写坏（字段缺失、类型不符）时
     是 rapidjson 断言 / 未定义行为，而不是丢弃该项。
  2. `on_position_changed()` 与 `set_position()` 都在 `getUnit() == NULL` 时 `return`/`continue`，
     但目标仓位与差量累计就在这一句之前/之后被跳过：执行单元创建失败时策略信号**静默丢失**，
     只有一条 `WTSLogger::error("Creating ExecUnit {} failed")`，没有重试也没有把 `_target_pos` 记下来等通道恢复。
  3. `set_position()` 末尾固定 `save_data()`（`WtDiffExecuter.cpp:641`），而 `save_data()` 是 private
     且没有失败处理，落盘失败（目录只读）不影响后续执行 —— 重启后差量与内存不一致。
- 期望：字段级校验 + 失败可见（告警计数 / 拒绝该代码）。
- 实际：静默。
- 处置：用例覆盖了 `load_data` 的「文件不存在 / 空内容 / 非法 JSON / 合约无效」四条入口；
  字段缺失输入属崩溃类，未纳入启用用例。

## 2026-10-29 独立复核与本机测试记录

本节依据源码、公开接口、测试定义和本机现有 coverage 二进制；未执行禁用崩溃用例，未改生产代码或测试。

### 逐项分类与处理

- **BUG-P02-001 — 待验证。** 买卖撤单缺 `_trader` 空值守卫为静态事实；`on_channel_ready()` 自身置位也为静态事实。但提交的复现需在没有 trader 的执行器上手工发送 ready，尚无有效 Runner/TraderAdapter 路径证据。修复可在 `setTrader(nullptr)` 清除 ready 状态，并在所有下单入口同时检查 `_trader`；若产品合同保证 ready 只能由已绑定 trader 发出，至少保留防御性检查。禁用用例为空体，本轮无崩溃运行证据。
- **BUG-P02-002 — 重复、撤回与待验证。** Arbi/Diff 的 `policy`/`default` 空指针链与 BUG-R10-001 同一问题，状态为**已合并（BUG-R10-001）**；Local 同形查找链仍待确认缺失策略是否属于允许输入。`createExeUnit("Snel")` 先检查拆分段数并返回空指针，因此 name 无点号的越界主张**已撤回**。修复方向：明确并在 `init()` 校验必需策略结构，同时让 `getUnit()` 保留判空和日志。DISABLED 测试仅声明输入形态，测试体为空。
- **BUG-P02-003 — 产品缺陷，本机复现。** `get_adjusting_flag()` 调用了 `_data_mgr->get_adjusting_flag()` 后仍无条件返回 0。旧测试固化了错误的 0，现已改为 `AdjustingFlagPropagatesDataManagerValue`；重建后单跑 FAIL，替身为 1、引擎为 0。显式初始化为无 manager 的默认 0 控制 PASS。修复时返回 manager 的结果并保留空 manager 默认值。
- **BUG-P02-004 — 待验证。** 未判空调用点可静态定位；当前 `init()`/Runner 会注入依赖，非零仓位、无 commodity 等形态是否违反初始化或配置合同未证。`DISABLED_NullHotMgrAndUnseededBaseDataPaths` 是空体，不是输入覆盖。修复方向：列明必需依赖并在初始化失败时拒绝；对外部/配置驱动对象查找和 split 结果做局部校验。未运行这些崩溃路径。
- **BUG-P02-005 — 静态逻辑缺陷成立。** Local、Arbi 都计算 `real_targets`，但派发循环仍遍历 `targets`，清理判断也按原始 map 进行。启用用例通过无插件工厂，不能动态观察执行单元接收值。修复方向：先确定组合代码和成员代码如何分别派发，再将变换 map 贯穿差量、清理和同步；补可观测执行单元的测试后才可验收行为。
- **BUG-P02-006 — 子项状态不同。** ① `load_data()` 对字段直接调用 RapidJSON `GetString/GetDouble`，缺成员或类型错误可能触发断言；现有加载用例只覆盖缺文件、空内容、语法错误和未知合约，不覆盖结构/类型损坏，旧文件格式合同待确认，状态待验证。② `getUnit()==NULL` 时更新被跳过；Diff 执行器会记创建失败日志，但是否应暂存/重试没有接口合同，状态待验证。③ `save_data()` 仅在文件创建成功时写入，创建/写入失败无错误返回或日志；持久化失败的恢复要求需明确，修复方向为失败可观测且不得报告保存成功。`ScaleAppliedOnlyAfterUnitExists` 失败是测试文件名写错：对象构造名固定 `p02_diff_exe`，保存文件按 `_name` 命名，断言却查 `p02_diff_scale`；不能据此认定生产写盘失败。

### coverage 运行结果

- 目标二进制：`build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore`；修正前过滤 `P02*.*` 为 30 项、28 PASS、2 FAIL、2 DISABLED。
- `P02DiffExecuterTest.ScaleAppliedOnlyAfterUnitExists`：旧断言查错文件名，已改为 fixture 实际对象名 `p02_diff_exe.json`；重建后单例 PASS。
- `P02EngineTest.PortContextQueriesWithoutInit`：fixture 覆盖 `transTimeToMin` 返回原值，已将不成立的 0 断言改为 930；重建后单例 PASS。
- 修正后的 `P02*.*` 共 31 条启用测试为 30 PASS / 1 FAIL；唯一失败是 `P02EngineTest.AdjustingFlagPropagatesDataManagerValue`，空 manager 默认值控制 PASS。另有 2 条禁用测试未运行；交易单元派发行为尚未全面验收。
