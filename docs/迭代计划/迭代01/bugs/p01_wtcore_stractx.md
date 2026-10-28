# p01_wtcore_stractx（WtCore 策略上下文）疑似缺陷

> 状态一律「待审核」；本迭代不修改 `src/**`。

## BUG-P01-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtCore/CtaStraBaseCtx.cpp`（`stra_exit_long` L1238-1240、`on_tick` L804-805）
- 相关测试: `src/TestUnits/coverage/p01_wtcore_stractx/p01_cta_order_cond_test.cpp` → `DISABLED_P01CtaCrashClass.unchecked_null_pointers_segfault`
- 输入/前置条件: 引擎只挂了 `IBaseDataMgr`（未 `init`，因此时段没有回写到 `WTSCommodityInfo::m_pSession`），对一个「已登记但没有时段」的合约调用 `stra_exit_long`；或 `_sig_map` 里有未触发信号时收到 tick。
- 期望行为: 取不到时段信息时应记录错误并跳过本次平多/信号触发。
- 实际行为: `commInfo->getSessionInfo()->offsetTime(...)` 与 `_engine->get_session_info(stdCode, true)->isInTradingTime(...)` 直接解引用返回值，无 NULL 判定 → 段错误。
- 复现步骤: 打开该 `DISABLED_` 用例（第 1 条语句）单独运行。
- 影响范围: 生产环境里品种时段一般在 `WTSBaseDataMgr` 装载时已绑定，所以正常链路不会命中；但策略上下文一旦被独立复用（回放/测试/新增品种漏配时段）就是崩溃。同类空指针在 `HftStraBaseCtx`/`SelStraBaseCtx` 的数据装配里应一并排查。
- 备注: 不阻塞本 lane 的覆盖率目标（启用用例已完全绕开该输入）。修复方向：两处加 NULL 守卫 + `log_error`。

## BUG-P01-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtCore/CtaStraBaseCtx.cpp`（`stra_get_bars` L1577、`stra_get_ticks` L1648、`stra_get_last_tick` L1657-1660）、`src/WtCore/WtEngine.cpp`（`get_tick_slice` L592-600）
- 相关测试: 同上 `DISABLED_` 用例第 3 组语句；其余历史切片接口在本 lane 中「未覆盖」
- 输入/前置条件: 未调用 `WtEngine::init` 的引擎（`_data_mgr == NULL`）+ 任意 `stra_get_ticks/stra_get_last_tick` 调用。
- 期望行为: 数据管理器缺失时返回 NULL 并给出可诊断的日志。
- 实际行为: `return _data_mgr->get_tick_slice(code, count);` 无守卫 → 段错误；`stra_get_last_tick` 同样直接透传。
- 复现步骤: 打开 `DISABLED_` 用例第 3 条语句。
- 影响范围: 策略侧任何历史数据接口误用（引擎未就绪 / 单元化部署漏配 store）都是崩溃而不是可控失败。
- 备注: 因为 `WtDtMgr` 需要真实 store（LMDB/HDF5 DLL）才能构造，本 lane 无法在启用用例里覆盖 `stra_get_bars/stra_get_ticks/stra_get_last_tick` 的正常返回路径，见 manifest 跳过清单。修复方向：三个入口先判 `_data_mgr`。

## BUG-P01-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtCore/CtaStraBaseCtx.cpp`（`stra_set_position` L1352-1359）
- 相关测试: 建议在 `p01_cta_order_cond_test.cpp` 的 `set_position_condition_is_skipped_when_target_equals_current` 上扩展
- 输入/前置条件: `stra_set_position("SHFE.rb9999", 5)`（合约/品种不存在）。
- 期望行为: 与 `stra_enter_long/short` 一致，先校验品种存在性，不存在则拒绝并记日志。
- 实际行为: `stra_set_position` 不做任何合约校验，直接 `sub_tick` + `append_signal` + `save_data`；信号会被写入 `datas.json`，随后只能在 `do_set_position` 里因 `commInfo == NULL` 静默 return，形成「已落地但永远无法成交」的僵尸信号。
- 复现步骤: 调用后即可在策略目录的 `datas.json` 中看到该 code。
- 影响范围: 合约换月/代码写错时仓位不会报错，只是不动作，容易掩盖策略配置错误。
- 备注: 不阻塞覆盖率。

## BUG-P01-004

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtCore/CtaStraBaseCtx.cpp`（`load_data` L315、L411）
- 相关测试: 本 lane 的 save/load 往返目标（因预算未落地，见 manifest 剩余项）
- 输入/前置条件: `stradata/<策略名>.json` 缺少 `positions` 或 `conditions` 成员（旧版本产物或人工编辑）。
- 期望行为: 与 `fund`/`signals`/`utils` 一样用 `HasMember` 守卫后再读取。
- 实际行为: 直接 `root["positions"]` / `root["conditions"]`，缺失时触发 rapidjson 断言（Debug）或读到垃圾值。同一层的 `pItem["volume"]`、`dItem["profit"]` 等成员也全部无 `HasMember` 判定，仅 `frozen`/`opentdate`/`maxprice` 等少数字段有。
- 复现步骤: 手写一个只含 `{}` 的 datas.json 后触发 `on_init`。
- 影响范围: 策略数据文件被外部工具改动过就可能异常；影响恢复流程健壮性。
- 备注: 该分支需要额外的落地夹具（fixture JSON + `on_init`），本 lane 因预算收口未提交，留给下一波。
