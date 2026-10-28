# p02_wtcore_engines2 缺陷登记（状态一律「待审核」，本轮未改任何生产代码）

模板见 `docs/迭代计划/迭代01/bugs/README.md`。

---

## BUG-P02-001 通道就绪后 buy/sell/cancel 直接解引用空 _trader（三个 Executer 同构）

- 状态：待审核
- 严重级别：崩溃（SIGSEGV）/ 缺陷类，影响 3 个执行器
- 位置：
  - `src/WtCore/WtLocalExecuter.cpp:328-392`（`buy` / `sell` / `cancel(uint32_t)` / `cancel(code,isBuy,qty)`）、`WtLocalExecuter.cpp:798-824`（`on_channel_ready`）
  - `src/WtCore/WtDiffExecuter.cpp:379-432` + `on_channel_ready`
  - `src/WtCore/WtArbiExecuter.cpp`（同一模式的四个下单入口）
- 复现（缺陷类，不是一个点）：
  1. `WtLocalExecuter exec(&factory, "exe", &reader)`，**从不调用 `setTrader`**（或 `setTrader(NULL)`）；
  2. `exec.on_channel_ready()` —— 该函数无条件把 `_channel_ready = true`；
  3. `exec.buy("SHFE.ag.2212", 5000.0, 1)` → `if(!_channel_ready)` 通过 → 执行 `_trader->buy(...)`，`_trader` 仍为 NULL。
  同类入口还有 `sell` / `cancel(localid)` / `cancel(stdCode,isBuy,qty)`；`WtDiffExecuter`、`WtArbiExecuter` 三份实现同构。
  另外 `on_position_changed` / `set_position` 里的风控限单判断写作 `if(_trader && !_trader->checkOrderLimits(...))` —— 说明作者知道 `_trader` 可能为空，下单入口却没有同样的护栏，属同一缺陷类的漏检。
- 期望：`_trader == NULL` 时下单入口返回空 `OrderIDs` / `false`；或 `on_channel_ready` 不在没有交易通道时置位。
- 实际：空指针解引用。
- 处置：本 lane 的启用用例只驱动 `_channel_ready == false` 的护栏分支，全部崩溃输入收在
  `p02_local_arbi_executer_test.cpp` 的 `P02ExecuterCrashClass.DISABLED_ChannelReadyWithoutTraderAndBadPolicyConfigs`。
- 收益：无（仅登记）。

---

## BUG-P02-002 `getUnit()` 未校验 policy 配置，非法配置直接崩而不是拒绝

- 状态：待审核
- 严重级别：崩溃（配置驱动，可被上线误配触发）
- 位置：`WtLocalExecuter.cpp:170-222`、`WtDiffExecuter.cpp:243-286`、`WtArbiExecuter.cpp:163-…`
- 复现（缺陷类三条）：
  1. `init()` 的配置里 **没有 `policy` 段** → `WTSVariant* policy = _config->get("policy")` 返回 NULL → 下一行 `policy->has(commID)` 空指针；
  2. 有 `policy` 但既没有该商品ID也没有 `default` → `cfg = policy->get(des)` 为 NULL → `cfg->getCString("name")` 空指针；
  3. `policy.default.name` 不含 `.`（写成 `"Snel"` 而非 `"Snel.Local"`) → `createExeUnit(name)` 里按 `.` 拆分后按下标取第二段，越界。
  注意 `init()` 对这三类配置都返回 `true`：配置校验完全没有收口，问题被推迟到第一笔仓位变动。
- 期望：`init()` 阶段拒绝非法配置并返回 `false`（或 `getUnit` 判空后返回空指针，让上层记日志跳过）。
- 实际：运行期空指针 / 越界。
- 处置：同上，收进 DISABLED_ 用例；本 lane 配置样本一律写成 `{"policy":{"default":{"name":"a.b"}}}` 的合法形状。

---

## BUG-P02-003 `WtEngine::get_adjusting_flag()` 丢弃数据管理器的返回值

- 状态：待审核
- 严重级别：逻辑错误（复权方式恒为「不复权」）
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
- 复现：用例 `P02EngineTest.AdjustingFlagIgnoresDataManager` 里注入的 `WtDtMgr` 替身返回 `1`，
  `WtEngine::get_adjusting_flag()` 仍然返回 `0`（已作为断言写死，修复后该断言需要一并翻转）。
- 期望：`return _data_mgr ? _data_mgr->get_adjusting_flag() : 0;`
- 实际：恒 0。影响所有走 `stra_get_bars`/复权读取的策略。

---

## BUG-P02-004 `WtEngine` 一组未判空的依赖解引用（空指针类）

- 状态：待审核
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
- 实际：静默空指针。
- 处置：本 lane 引擎用例始终注入 `WtDtMgr` 替身与空基础数据管理器，只走「查不到商品 → 早退」的分支；
  非零仓位的 `do_set_position` 与 `sub_tick` 收在 `P02EngineCrashClass.DISABLED_NullHotMgrAndUnseededBaseDataPaths`。

---

## BUG-P02-005 `WtLocalExecuter::set_position()` 的组合合约结果被算完就丢

- 状态：待审核
- 严重级别：逻辑错误（`groups` 配置不生效）
- 位置：`src/WtCore/WtLocalExecuter.cpp:505-592`
- 描述：函数先按 `_groups` 把成员合约轧成 `real_targets[gpInfo->_name] = gpQty`，并从成员上扣减；
  随后第一个派发循环遍历的却是入参 `targets`（不是 `real_targets`），第二段清理循环同样只看 `targets`。
  因此 `real_targets` 的全部计算结果无人使用，`groups`（如 `SHFE.ags: {ag:1, au:2}`）在本地执行器里没有任何效果。
- 期望：派发与差量清算应基于 `real_targets`，或明确删除该计算。
- 实际：死代码；用户配置被静默忽略。
- 处置：`P02LocalExecuterTest.SetAndDeltaPositionWithoutExecutableUnits` 里配置了 `groups` 以覆盖该计算路径
  （结果不可外部观测，因为单元创建失败），逻辑修正需要产品决策，本 lane 只登记。

---

## BUG-P02-006 `WtDiffExecuter` 的 JSON 读取与仓位丢失都是静默的

- 状态：待审核
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
