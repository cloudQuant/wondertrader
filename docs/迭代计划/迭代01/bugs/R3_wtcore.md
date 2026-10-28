# R3 缺陷登记（WtCore 第三波）

lane: R3 · 2026-10-27 · 全部条目 `状态: 待审核`，未改任何生产代码。
证据用例都在 `src/TestUnits/coverage/q01_wtcore3/`（已过真实构建口径 `-fsyntax-only` 门禁，运行由主控统一做）。

## BUG-R3-001 风控频率限流的键空间不一致：缓存按交易所原始代码写、按标准代码查
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtCore/TraderAdapter.cpp` L517（`_order_time_cache[entrust->getCode()]`，而 L498 已把 code 改写成 `cInfo->getAltCode()`）、L1286（`_cancel_time_cache[ordInfo->getCode()]`）↔ L609-661 / L544-596（`checkOrderLimits` / `checkCancelLimits` 的入参）、L1259（`doCancel` 传的是 `stdCode`）
- 现象（实测）: `r3_trader_risklimit.cpp` 的 `order_freq_window_blocks_only_for_the_raw_code_key`
  连下 4 单后，`checkOrderLimits("SHFE.ag.2512")` 恒为 `true`，只有 `checkOrderLimits("ag2512")` 返回 `false`；
  封禁落进 `_exclude_codes` 的是 `"ag2512"`，因此 `isTradeEnabled("SHFE.ag.2512")`、`buy/sell/doCancel` 全都不受影响。
  `cancel_freq_window_is_keyed_by_order_raw_code` 撤单侧同理。
- 影响: 「N 秒内最多 N 次报单/撤单」这条风控在真实调用链上**不生效**（柜台回报也是标准代码，所以整条链都用标准代码查询）。
- 与既有登记的关系: lane p11 的 `P11-003` 记为「频率窗口这条分支实测不可达」。本轮实测**可达**——用缓存键（原始代码）查询即达，
  所以「不可达」只对生产调用方成立；建议把 P11-003 改写为「键空间不一致」，不要按不可达处理。
- 修复建议（待评审，勿在本波实施）: 统一在 `doEntrust`/`doCancel` 用标准代码入缓存，或让两个 `check*Limits` 同时查两个键。

## BUG-R3-002 频率窗口阈值算术与配置名不符（`cnt >= boundary` 配 `times > boundary`，且多减 1）
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtCore/TraderAdapter.cpp` L636-643（下单）与 L571-578（撤单）
- 现象: `TimeCacheList` 长度 `cnt >= *_times_boundary` 才进窗口判定，进窗口后又用 `times = cnt - sIdx - 1` 与
  `> *_times_boundary` 比较。实测 `order_times_boundary=2` 时：3 笔在 60 秒内**放行**，第 4 笔才封禁
  （`r3_trader_risklimit.cpp:order_freq_window_blocks_only_for_the_raw_code_key`）。
  即实际语义是「窗口内允许 boundary+1 笔」，而不是配置名暗示的「最多 boundary 笔」。
- 影响: 与交易所报单频次罚则（例如 1 秒 N 次）对照时容易低估 1 次；同一函数里 `>=` 与 `>` 混用也没有注释说明。
- 修复建议（待评审）: 明确 `times = cnt - sIdx` 并统一用 `>=`，或把配置名改成 `*_times_boundary_plus_one` 并加注释。

## BUG-R3-003 频率缓存的「过期裁剪」支路无法被任何测试触发（缺可注入时钟）
- 状态: 待审核
- 严重级别: Minor（测试可达性 / 长跑内存）
- 位置: `src/WtCore/TraderAdapter.cpp` L588-591、L653-656（`cache.erase(cache.begin(), tit)`）
- 现象: 缓存时间戳只在 `doEntrust`/`doCancel` 内部由 `TimeUtils::getLocalTimeNow()` 写入，
  公开接口无法注入历史时间点，因此 `tit != cache.begin()`（即窗口内已滑出旧条目）这条支路在单元层不可达。
  生产上 `TimeCacheList` 会随运行时长持续变长（注释自己也写了「不然随着时间推移, vector 长度会越来越长」），
  而唯一的清理入口恰好是这条不可达支路。
- 需要的接缝（留给主控）: `TimeUtils::getLocalTimeNow()` 的可替换 seam（函数指针 / 测试注入），或把 `*_time_cache` 暴露成受保护的写入接口。

## BUG-R3-004 `stra_exit_long` 与 `stra_exit_short` 的可平量口径不对称
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtCore/CtaStraBaseCtx.cpp` L1238-1243（多头平仓：读 `bOnlyValid=true` 的可平持仓，收盘那根 bar 例外）↔ L1295-1297（空头平仓：`stra_get_position(stdCode)` 全量，不看冻结量）
- 现象: T+1/冻结语义只作用在多头侧；空头侧即便 `_frozen` 非 0 也按全量截断。
  `r3_cta_position_persist.cpp:t1_entry_freezes_position_until_next_session_begin` 证明 `_frozen` 会在开仓时累积，
  而 `condition_os_cs_and_sp_fire_on_matching_tick` 里空头平仓条件单走的是不看冻结的全量口径。
- 影响: 当前 `canShort()` 只在 `TM_Both` 为真、而 `isT1()` 只在 `TM_LongT1` 为真，两者互斥，所以现实中不共现——
  但这是「靠调用方保证」的隐式约束（L1846 的注释也写着「其他地方要保证，空头持仓的话，_frozen 要为 0」），
  没有断言或代码保护。建议要么在 `stra_exit_short` 也走 `bOnlyValid`，要么把注释升级为断言。

## BUG-R3-005 `do_set_position` 反手时不清零 `_dynprofit`（全平才清）
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtCore/CtaStraBaseCtx.cpp` L1477-1479（`if (decimal::eq(pInfo._volume, 0)) pInfo._dynprofit = 0;`）与 L1507（按剩余量等比缩放）、L1535-1563（反手新建明细）
- 现象: 平仓循环结束后若仍有 `left > 0`（反手），新建的 `DetailInfo` 不会重置 `pInfo._dynprofit`，
  残值要等下一条 tick 走 `update_dyn_profit`（L752-793）才被覆盖；期间 `save_data` 落盘的 `dynprofit` 与
  `stra_get_position_profit` 读到的是旧仓残余值。反手同时方向翻转（多空互换），残值符号也不再成立。
- 备注: 本轮用例（`set_position_through_zero_reverses_side_with_single_detail`）只断言了手数/明细，
  **没有**对 `dynprofit` 下断言——因为缺一次「反手后、喂下一条 tick 前」的落盘读回；若评审认为这是缺陷，
  请把它裁定为需要补一条用例，而不是把现有断言改成实现返回值。

## BUG-R3-006 `on_tick` 里 session 信息无判空（崩溃族，同 p01 已登记的 DISABLED 条目）
- 状态: 待审核
- 严重级别: Blocker（崩溃）
- 位置: `src/WtCore/CtaStraBaseCtx.cpp` L804-805（`WTSSessionInfo* sInfo = _engine->get_session_info(stdCode, true); if (sInfo->isInTradingTime(...))`）
- 触发条件: `_sig_map` 里有该合约的待落地信号，且该品种未配时段（`get_session_info` 返回 NULL）→ 空指针解引用。
  本轮**没有**新增 `DISABLED_` 用例（p01 的 `DISABLED_P01CtaCrashClass.unchecked_null_pointers_segfault` 已覆盖同一崩溃类），
  仅在此登记触发条件，供裁定是否值得再开一条同族用例。
- 相邻的「非崩溃但静默」支路: 时段存在但 `m_tradingTimes` 为空时，`WTSSessionInfo::timeToMinutes`
  先判 `empty()` 返回 `INVALID_UINT32` ⇒ `isInTradingTime` 恒 false ⇒ **信号永远不落仓**，
  且 `on_tick` 连一条日志都不打（L805-817 整段静默跳过，`_sig_map` 保留）。
  本轮用例 `r3_cta_position_persist.cpp` 必须在 session 上 `addTradingSection(900,1130)` 才能让 `do_set_position` 跑起来，
  就是这条支路的直接证据；建议在时段表为空时至少打一条 WARN。

## BUG-R3-007 `WtCtaEngine::handle_pos_change` 在引擎未 init 时也会落盘组合层数据
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtCore/WtCtaEngine.cpp` L429-488（尤其 L452 建仓 + L477-478 `append_signal`/`save_datas`）
- 现象: 策略侧 `do_set_position(..., bFireAtOnce=true)` 无条件把增量提交给引擎，引擎随后 `save_datas()`
  往 `WtHelper::getPortifolioDir()/datas.json` 写快照；此时 `_port_fund` 仍为 NULL（`save_datas` L372 有判空，
  不会崩），但写出的 `datas.json` 里没有 `fund` 段——回读侧 `load_datas`（L473+）会创建 `WTSPortFundInfo` 并按缺省值起算。
  即「未登录/未就绪」状态也能污染组合层落盘文件。
- 证据: `r3_cta_position_persist.cpp` 的 4 条 `bFireAtOnce=true` 用例（`engine.port_pos()` 变化即可证明提交发生），
  对照 `enum_position_for_execute_marks_signal_triggered_and_skips_portfolio`（`_triggered` 后不再提交）。
- 待评审: 这是「设计如此」还是应当加 `_ready` 门禁，请评审裁定；本轮只登记。

## 本轮被接缝/平台挡住、需要主控给接口或进排除清单的面
（详见 `manifests/R3_wtcore.md` 的同名章节，这里只列条目）
- `CtaStraBaseCtx.cpp` L1577-1675 历史切片族：需 `WtEngine::_data_mgr`（`WtDtMgr`），只在 `WtEngine::init()` 里赋值，而 `init` 会拉起线程与共享内存 → 本波禁止。
- `WtEngine.cpp` L285-361 / L1086-1130 / L1143-1186：`init` 装配 + `task_loop`/`_pool` 线程 + `init_riskmon` 的 `DLLHelper` 插件加载（BUG-L00-002：hidden SHARED 无 C++ 链接缝）。
- `WtCtaEngine.cpp` L61-114（`run` 起 `WtCtaRtTicker`）、L116-235（`init`/session 广播）、L490-560（`on_tick` 订阅分发，需 `_ctx_map`+`_exec_mgr` 真装配）。
- `TraderAdapter.cpp` L99-208 `init` 的厂商柜台装配（CTP/XTP/Femas 真实 dll）→ 覆盖排除清单候选。
- `HftStraBaseCtx.cpp` 全量 987 行（missed 356）：**本波预算内未开工**，队列/快速通道分支需先接 `q01_hft_ctx_test.cpp` 的替身；已排下一波第一位。
