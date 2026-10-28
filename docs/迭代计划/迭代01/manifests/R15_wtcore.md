# R15_wtcore 用例清单（lane R15，WtCore 第七波：HftStraContext/Sel 落地/工厂扫描）

构建归属：`CoverageTests_wtcore`（目录 `src/TestUnits/coverage/q01_wtcore3/`，前缀 `r15_`，未新建目录、未改 CMakeLists）。
门禁：3 个文件全部用真实编译行（`ninja -t commands ...CoverageTests_wtcore.dir/q01_wtcore3/r3_cta_position_persist.cpp.o`，把 `-o/-c` 换成 `-fsyntax-only`）跑通，`-Werror` 下 exit 0（3/3）。
替身只引用 WtCore 一族：`q01_sel_env.h`（BaseDataMgr/HotMgr/SelEngine/SelCtx/Q01TempEnv）+ 本 lane 自带的
`R15HftEngine`（裸 WtHftEngine，接 bd/hot，不 init/run）、`R15SpyStra : HftStrategy`（回调计数替身）、
`R15HftCtx : HftStraContext`（仅暴露 protected inline 标签链与 `_tick_subs`，无宏）。

| 文件 | 用例数 | 启用 | DISABLED | 门禁 |
|---|---|---|---|---|
| `r15_hft_stra_context.cpp` | 14 | 12 | 2 | exit 0 |
| `r15_sel_persist.cpp` | 15 | 15 | 0 | exit 0 |
| `r15_mgr_executers.cpp` | 9 | 9 | 0 | exit 0 |
| 合计 | 38 | 36 | 2 | 3/3 |

## 用例 → 生产文件:行段（行号取自本 lane 实读的源码）

### src/WtCore/HftStraContext.cpp + HftStraContext.h（280 行，13 个转发方法全启用打满）
| 用例 | 行段 | 说明 |
|---|---|---|
| strategy_pointer_roundtrip | HftStraContext.h L46-52 | `set_strategy/get_stragety` inline |
| null_strategy_events_only_touch_base | .cpp L47-52, L81-93(基类 funds), L105-118, L171-176, L215-235, L247-251, L261-265, L279-284 | `_strategy==NULL` 时每个事件只走基类；ud 脏/刷两态可证伪 |
| on_init_and_session_callbacks_forward_to_strategy | L47-52, L62-67, L81-89 | this 指针 + uTDate 原样转发 |
| session_end_forwards_date_then_base_writes_fund_line | L81-93 | 策略先行、基类 funds.csv 数值确定 |
| on_tick_without_subscription_skips_strategy | L105-118 | `_tick_subs` 空 ⇒ 不转发；基类不记价 ⇒ 价仍 0 |
| on_tick_subscribed_forwards_same_pointer | L105-118 | 订阅命中 ⇒ 原指针转发；未订阅合约不计数 |
| on_bar_forwards_code_period_times_and_bar | L171-179 | code/period+times/bar 指针 |
| ordque_orddtl_trans_forward_when_strategy_set | L121-127, L137-143, L153-159 | 三类盘口数据转发 + 摘策略后计数冻结 |
| on_trade_forwards_inner_code_and_order_tag | L192-203 | innerCode(空 code_map⇒原样)+getOrderTag("") 转发；基类账本 FIFO 全平 |
| on_order_and_entrust_forward_tag_and_cancel_flag | L215-229, L279-284 | totalQty/leftQty/isCanceled/message 入参 |
| on_position_forwards_only_to_strategy | L247 前的 on_position 段 | 不调基类 ⇒ ctx 账本保持空 |
| channel_events_forward_and_flush_userdata_only_when_dirty | L247-251, L261-265 | ready/lost 转发 + 基类 ud 脏才写 |

### src/WtCore/HftStraBaseCtx.h（inline 订单标签链；启用子集 + 2 条 DISABLED）
| 用例 | 行段 | 状态 |
|---|---|---|
| （null_strategy/on_trade/on_order 各案内的）getOrderTag 空表早退 | L625-637 | 启用 |
| DISABLED_order_tag_set_get_erase_end_to_end | L646-649, L656-668, L774 | **DISABLED**：setUserTag push_back 进 capacity==0 的 circular_buffer ⇒ assert-abort/UB（BUG-R15-001），解开即崩宿主 |
| DISABLED_order_tag_lookup_miss_returns_neighbor_tag | L625-637, L656-668 | **DISABLED**：同上触发条件；失配查询返回后继标签（BUG-R15-002）的期望写在 bugs 里 |

### src/WtCore/SelStraBaseCtx.cpp（15 条全启用）
| 用例 | 行段 | 说明 |
|---|---|---|
| init_outputs_reopen_keeps_single_header | L59-160（L64 目录创建 + 各 csv 的 isNewFile/seek_to_end 双分支） | 同名二次 on_init 不重复表头 |
| userdata_flushed_by_callback_then_reloaded_on_init | L170-196（root L173、PrettyWriter L190）、L198-227（read L211） | 置脏不写⇒on_session_begin 刷⇒新实例 on_init 回读；二次刷不增长 |
| load_userdata_missing_file_returns_early | L204-207 | !exists 早退 |
| empty_session_end_writes_zero_fund_line_and_three_sections | L379-482 空表三段 + fund 行 | 落地数值/键名确定 |
| dynprofit_update_saved_into_json_and_csv | L549-585(on_tick 价表+update_dyn_profit)、L692-714(on_session_end 落 positions/funds + save_data) | 2*(5350-5200)*10=3000 全链路确定 |
| saved_dynprofit_reloaded_into_fund | L265-340（仓位/明细回读）+ L1029-1043 | avgpx 5200、fund(0/1/2/3/9) 五个口径 |
| stra_set_position_appends_signal_and_persists | L734-770、L772-784(append_signal⇒save_data) | signals.csv `SHFE.ag.2512,2,0,20250110000000000,TT`；等目标早退 |
| signal_recovered_from_file_and_saved_back | L345-375（信号恢复 + stra_sub_ticks）| 时段为空⇒不触发 do_set_position（q01 崩溃纪律）；三段原样落回 |
| signal_of_expired_contract_is_skipped | L352-357 continue 分支 | `"signals": {}` 回读证明未恢复 |
| fund_fees_restored_only_when_tdate_matches_engine | L250-262 | 同/异 tdate 两支 + flag default（L1042） |
| expired_position_zeroed_but_closeprofit_counted | L272-296 isExpired 链 | volume→0、dynprofit→0、closeprofit 进总和 |
| broken_json_and_empty_file_are_ignored | L238-246 | parse error / 空文 / 无文 三支 |
| frozen_position_released_on_next_session_begin | L604-626(on_session_begin 冻结释放) + L286-290(frozen 回读) | 可用 4⇒5，总量恒 5 |
| on_schedule_saves_state_and_emits_autoexit_signal | L587-633（save_data→调度→to_clear→append_signal） | 调度期读 932、读回 931；autoexit 行确定 |
| four_log_levels_do_not_touch_strategy_files | L1045-1061 | 四级转发后策略侧文件零变化 |

### src/WtCore/WtExecuterFactory.cpp / CtaStrategyMgr.cpp / HftStrategyMgr.cpp / ParserAdapter.cpp / WtLocalExecuter.cpp（9 条全启用）
| 用例 | 行段 |
|---|---|
| executer_factory_missing_dir_returns_false | WtExecuterFactory.cpp L17-21 + 三个 create* 的 find==end（L59-63 等） |
| executer_factory_scan_skips_dirs_text_and_unloadable_libs | L23-44（fs::path、目录 continue、非 dylib continue、dlopen 失败 continue） |
| executer_factory_free_library_when_creator_symbol_absent | L46-53（含 **L52 free_library**；用 build-cov 现成 libWtIndexFactory.dylib，nm 验证 createExecFact/createStrategyFact 导出=0） |
| cta_strategy_mgr_missing_dir_and_name_shapes | CtaStrategyMgr.cpp L30-34 + createStrategy 两形态 find==end（L96-99、L112-116） |
| cta_strategy_mgr_scan_free_library_path | L39-66（**L39 fs::path** + **L64 free_library** 同 Hft 形状） |
| hft_strategy_mgr_scan_free_library_path | HftStrategyMgr.cpp L30-34、L39-66（**L39/L64**）+ 两个创建口空表 |
| parser_adapter_initext_guard_and_log_forward | ParserAdapter.cpp L45-47（api NULL 早退）+ **L368-372 handleParserLog**（_stopped==false ⇒ log_dyn_raw("parser","")） |
| local_executer_null_init_and_empty_channel_events | WtLocalExecuter.cpp L75-78（init NULL 早退）、**L803/L836/L879** 三把 SpinLock（空 _unit_map 遍历）、L400-409 writeLog |
| local_executer_getunit_via_on_tick_over_empty_unit_map | L170-186（getUnit 全头 + **L186 SpinLock**）+ L667-672（on_tick bAutoCreate=false 早退） |

## 留白面（不硬闯，沿用/补充 R10 接缝账）
| 面 | 本轮 UNCOVERED 行 | 处置 |
|---|---|---|
| WtCtaTicker/WtHftTicker/WtSelTicker run 循环（锁/睡眠/getDateTime） | ~17 行（列出的全部） | 线程禁用 ⇒ BUG-R15-006 |
| WtHftEngine run/init + marker.json 落地段 | ~45 行（L56/L61/L81-110） | reader/adapter 双缝 ⇒ BUG-R15-006 |
| WtLocalExecuter getUnit 建单元成功后的循环体（L196-213 的 create/init 段） | ~30 行 | BUG-R10-002 工厂缝未解 |
| SelStraBaseCtx on_bar/调度回调体系 | 不在此轮 R15 的 UNCOVERED 行内 | 不动 |
| HftStraBaseCtx.h trader 链（stra_buy/sell/enter/exit/cancel/get_undone 等） | 列于简报但不属本行段 | R10 已登 ~260 行接缝，未重复登记 |

合计留白 ~90 行 + R10 旧账不变。

## 断言口径备注
- 所有期望值由本用例自己喂的输入推导（5350-5200、×10 乘数、20250110*1e9 的 gentime、0 价表 ⇒ sigprice 0）。
- DISABLED 两条的触发条件（capacity-0 push_back）与判定依据写在 bugs/R15_wtcore.md BUG-R15-001。
- `update_dyn_profit(…,NULL)`、`on_tick(…,NULL)` 仅在 `_pos_map` 为空时安全（R10 L908-909 实测口径），本 lane 全部按该前提使用。
