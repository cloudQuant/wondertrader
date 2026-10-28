# lane R27 交付清单 — ParserXTP 行情解析器（收发族·第七波）

- 生产目标：`src/ParserXTP/ParserXTP.cpp`（二进制 `CoverageTests_t_ParserXTP`，target 由目录名 `t_ParserXTP` 自动派生，未改 `src/TestUnits/coverage/CMakeLists.txt`）
- 用例文件（全部在已登记目录 `src/TestUnits/coverage/t_ParserXTP/`，前缀 `t27_`）：
  - `t27_parserxtp_api_seam_test.cpp`（32 条启用 + 3 条 DISABLED）— 登录/订阅状态机
  - `t27_parserxtp_data_state_test.cpp`（16 条启用）— 登录状态回写行情/逐笔 + `OnTickByTick` 深圳分支
  - `t27_seam.hpp`（夹具头，只被上面两个 TU 包含，不是用例，不进构建表；CMake 只 GLOB `*.cpp`）
- 本 lane 启用 48 条 / DISABLED 3 条；`t_ParserXTP` 目录合计实跑 61 条（含 T1 lane 的 12 条 + gate 占位 1 条）。
- 所有行号是本轮直读 `src/ParserXTP/ParserXTP.cpp`（696 行）与 `ParserXTP.h`（131 行）的实录。

## 访问缝（先说清楚，主控要否掉就删这两个 TU + 头文件即可，别的一律不依赖）

`src/ParserXTP` 剩下的 0% 行**全部**挂在两个私有变量上：`m_pUserAPI`（只能由 `init()` 从 dlopen 的厂商库拿）
与 `m_uTradingDate`（只能由 `DoLogin()` 置位）。实测本机没有 macOS 版厂商库：
`src/API/XTP2.2.32.2/` 只有 `linux/libxtpquoteapi.so`、`x64/xtpquoteapi.dll`、`x86/xtpquoteapi.dll` 三份，
`.../bin/TestUnits/` 下没有任何 `lib*.dylib` 可被 `DLLHelper::load_library` 命中 → `init()` 必崩（T1-XTP-01），
所以「用真 init 把状态灌起来」这条路是死的。

采用**只读自 `ParserXTP.h` 的布局镜像**（`t27_seam.hpp` 的 `ParserXTPMirror`：同一份基类列表 + 同一份成员声明序列）
把 `init()` 本来要写的 9 个配置字段 + api 指针灌成确定值，之后**只走公开入口**驱动：
`connect / subscribe / disconnect / release / isConnected / OnDepthMarketData / OnTickByTick / OnSub* / OnError / OnDisconnected`，
厂商侧调用（`Login / SetHeartBeatInterval / SetUDPBufferSize / SubscribeMarketData / SubscribeTickByTick /
GetTradingDay / GetApiLastError / RegisterSpi / Release`）全部落到本 TU 的 `FakeApi`。

四条边界，都实测过：
1. 不用 `#define private public`，不改 `src/**` 任何产品代码，不改 CMake。
2. `static_assert(sizeof(ParserXTPMirror) == sizeof(ParserXTP))` 是编译期第一道闸。
3. `XtpSeamF::SetUp()` 在写任何东西**之前**先用 11 个锚点断言镜像槽位（`m_sink/m_pBaseDataMgr` 由公开
   `registerSpi` 写入、`m_uTradingDate/m_iRequestID/m_pUserAPI` 由构造函数钉死、`m_fitSHSubs/m_fitSZSubs`
   公开 `subscribe` 填充、`m_hInst/m_funcCreator/_thrd_worker/_worker` 应为空）；对不上就 ASSERT 红，不会静默写坏内存。
4. 不 bind 端口、不碰真实柜台、不起线程。`connect()` 会拉起 `while(true)` 不可回收线程（T1-XTP-02），
   所以夹具把 `_thrd_worker` 预置成非空、让 `ParserXTP.cpp:146` 的判空为假——**这条是被测产品的分支，不是绕开它**；
   真起线程那条留在 `DISABLED_`。唯一一处 sleep 在产品自己的 lambda 里（`ParserXTP.cpp:193` 固定 2s），
   由 `pump()` 同步跑掉，用例不等回调。

## `t27_parserxtp_api_seam_test.cpp` → `src/ParserXTP/ParserXTP.cpp`

| 用例（TEST 名） | 覆盖行段 |
|---|---|
| `XtpSeamF.mirror_head_slots_track_ctor_and_register_spi` | L81-86（构造列表）、L690-696（registerSpi）、L592-607（未登录缓存 + `SSE.`/`SZSE.`/其他前缀三分支） |
| `XtpSeamF.mirror_tail_slots_are_the_objects_the_parser_uses` | 同上 L96-105 的等价落点 + L127（`_asyncio` 空转） |
| `XtpLoginF.login_touches_nothing_when_api_pointer_is_null` | L142-146、L160（connect）、L446-450（DoLogin 判空早退，本方法此前 0%） |
| `XtpLoginF.login_minus_one_logs_connect_failure_and_defers_the_event` | L452-460、L466、L468-478 的 -1 支路（同步段） |
| `XtpLoginF.login_minus_one_pump_delivers_connect_event_carrying_the_raw_code` | L462-464（失败 lambda，missed function 之一） |
| `XtpLoginF.login_other_failure_sends_one_sync_and_one_posted_connect_event` | L469-476（非 -1 支路：同步 `WPE_Connect(0)` + post + 日志） |
| `XtpLoginF.login_failure_without_sink_stays_silent_and_posts_nothing` | L455-457 的 `m_sink` 假支路、L29-30（write_log 空 sink 守卫） |
| `XtpLoginF.login_error_message_bytes_reach_the_sink_unchanged` | L466 + L27-37（GBK 定长 `error_msg` 原样透传，A-07 的编码坑） |
| `XtpLoginF.login_forwards_every_seeded_config_field_to_the_vendor_call` | L452-454（心跳/UDP 缓冲/Login 六参） |
| `XtpLoginF.login_success_sets_trading_date_and_starts_subscription_before_pump` | L479-494（成功支路：`strToTime` 换算 + 日志 + 同步 DoSubscribeMD） |
| `XtpLoginF.login_success_pump_delivers_connect_then_login_in_that_order` | L482-488（成功 lambda，带 `if (m_sink)` 的那个） |
| `XtpLoginF.login_success_with_dashed_trading_day_corrupts_the_trading_date` | L57-71（strToTime）× L481 → BUG-T27-001 |
| `XtpLoginF.login_success_with_colon_separated_trading_day_strips_only_colons` | L57-71、L70（convert::to_uint32 正常面） |
| `XtpLoginF.connect_returns_true_even_when_login_failed_and_never_creates_a_worker` | L142-148、L160（worker 判空为假的分支） |
| `XtpLoginF.subscribe_md_strips_exchange_prefix_and_routes_each_exchange` | L498-512、L507（裸 ticker 下发）、L552-554 |
| `XtpLoginF.subscribe_md_order_is_market_data_then_tick_by_tick_per_exchange` | L512-534（SH 先 md 后 tbt）、L554-566（SZ 同序） |
| `XtpLoginF.subscribe_md_market_data_failure_logs_sse_and_still_subs_tick_by_tick` | L513-517、L555-559（两处 md 失败文案）+ L519-534 成功侧仍进 |
| `XtpLoginF.subscribe_md_tick_by_tick_failure_logs_its_own_line` | L525-529、L567-571（两处逐笔失败文案） |
| `XtpLoginF.subscribe_md_with_empty_buffers_touches_the_api_zero_times` | L500-542 两个 `if(!codeFilter.empty())` 假支路、L542-578 同 |
| `XtpLoginF.subscribe_md_clears_only_its_local_copy_so_buffers_survive` | L536、L578（`codeFilter.clear()` 清副本）→ BUG-T27-002 |
| `XtpLoginF.second_login_resubscribes_the_very_same_codes` | L445-495 整链跑两遍 + L498-582 跑两遍（重复订阅实证） |
| `XtpOnlineF.post_login_subscribe_sends_market_data_but_never_tick_by_tick` | L608-623、L625-650（登录后直订，此前 0% 的 65 行）→ BUG-T27-003 |
| `XtpOnlineF.post_login_subscribe_splits_two_exchanges_into_two_calls` | L611-623、L625-650、L652-677（两交易所各一次） |
| `XtpOnlineF.post_login_subscribe_drops_foreign_prefixes_without_any_call` | L613-622 的非 SSE/SZSE 前缀、L625/L652 两个空集合早退 |
| `XtpOnlineF.post_login_subscribe_accumulates_into_member_buffers` | L615-621（直订同时写成员集合）+ 与 L498-582 的联动 |
| `XtpOnlineF.post_login_subscribe_failure_logs_the_api_return_code` | L638-642（SZSE 失败文案） |
| `XtpOnlineF.post_login_subscribe_logs_sse_failure_on_its_own_line` | L638-642（SSE 失败文案，最后一块可补的 0% 行） |
| `XtpOnlineF.post_login_subscribe_without_api_buffers_only` | L635 的 `m_pUserAPI` 假支路、L649/L676（`delete[]`）→ BUG-T27-008 |
| `XtpOnlineF.disconnect_unregisters_spi_releases_once_and_nulls_the_pointer` | L163-173（持指针分支 L166-170，此前 0%） |
| `XtpOnlineF.second_disconnect_is_a_no_op` | L165 的第二次判空为假 |
| `XtpOnlineF.release_routes_through_disconnect_and_keeps_the_io_context_idle` | L137-140 + L163-173 |
| `XtpSeamF.is_connected_and_disconnect_pair_without_an_api_pointer` | L685-688、L163-173、L137-140（空指针侧收口） |

## `t27_parserxtp_data_state_test.cpp` → `src/ParserXTP/ParserXTP.cpp`

| 用例（TEST 名） | 覆盖行段 |
|---|---|
| `XtpData27.tickbytick_szentrust_looks_up_szse_but_keeps_the_master_exchange` | **L215-218（深圳 else 支路，本目录唯一不依赖私有缝的 0% 行）**、L220-227、L229-256 |
| `XtpData27.tickbytick_sztrade_cancel_branch_marks_szse_master_exchange` | L215-218 + L257-282（成交换算）、L271-272（side/ttype）→ BUG-T27-005 |
| `XtpData27.tickbytick_szse_unknown_contract_logs_the_szse_key` | L215-226（SZSE 拼键 + `Instrument SZSE.xxx not exists...` 全文） |
| `XtpData27.tickbytick_szentrust_side_digit_falls_outside_the_bsdirect_enum` | L215-218、L233-256（L247-248 的 side/otype 裸强转）→ BUG-T27-005 |
| `XtpData27.tickbytick_sztrade_match_flag_collides_with_the_lend_direction` | L215-218 + L257-282（L271 `side=(WTSBSDirectType)trade_flag`）→ BUG-T27-005 |
| `XtpData27.tickbytick_state_type_is_dropped_by_both_branches` | L233/L257 两侧同时为假（`XTP_TBT_STATE`）→ BUG-T27-007 |
| `XtpData27.tickbytick_trading_date_comes_from_the_message_not_the_login` | L229-231、L242-244、L266-268（trading_date 与快照不同源） |
| `XtpData27.stock_category_skips_the_settlement_block` | L332-339 的**假支路**（非期货不写结算/持仓）+ L324-330、L341-354 |
| `XtpData27.level_one_queue_size_is_clamped_to_the_50_slot_array` | L360-386、L388-414（`std::min(count,50)` 与 `volumes[50]` 边界） |
| `XtpData27.subscribe_reports_treat_null_error_info_as_success` | L421-424、L434-437（NULL error_info）、L584-590（IsErrorRspInfo 三点）、L419-428、L432-441 |
| `XtpOnlineF.depth_market_data_after_login_writes_the_login_trading_date` | L329（`trading_date = m_uTradingDate`，T1 只能断言 0）+ L292-319 |
| `XtpOnlineF.level_one_queue_after_login_inherits_the_login_trading_date` | L368、L396（两个队形的 trading_date）+ L360-414 |
| `XtpOnlineF.tickbytick_after_login_still_uses_the_message_date` | L215-218 + L229-244（登录后逐笔仍取报文日期） |
| `XtpLoginF.corrupt_trading_day_leaks_into_snapshot_and_queue_records` | L481 → L329/L368/L396 的脏值传导（BUG-T27-001 的落库证据） |
| `XtpLoginF.on_disconnected_relogin_runs_only_when_the_context_is_pumped` | **L191-195（重登录 lambda，missed function 之一）** + L445-495 二次登录 + L498-582 |
| `XtpLoginF.on_disconnected_relogin_bails_out_when_the_api_is_gone` | L191-195 + L447-450 |

## DISABLED（3 条，实测判决见文末，打开即崩/abort，故不启用）

| 用例 | 不启用的原因 | 实测 |
|---|---|---|
| `XtpLoginF.DISABLED_init_dereferences_null_creator_when_vendor_dylib_is_absent` | `init()` L119-131：`load_library` 返回 NULL → `get_symbol` 返回 NULL → L131 调用空函数指针。根因由 T1-XTP-01 登记，这里只补可复现入口 | `--gtest_also_run_disabled_tests` 单跑 rc=139（SIGSEGV） |
| `XtpLoginF.DISABLED_posted_login_handlers_deref_sink_nulled_after_post` | L462-464 / L471-473 两个 lambda **不带** `if (m_sink)`（对比 L482-488 成功分支带）；入队时判定为真、执行前被 `registerSpi(nullptr)` 摘掉 → NULL 解引用。见 BUG-T27-004 | 单跑 rc=139（SIGSEGV） |
| `XtpSeamF.DISABLED_connect_without_pre_seeded_worker_starts_unstoppable_thread` | `_thrd_worker==NULL` 那条会起 `while(true)` 线程（L150-157），`release/disconnect` 都不回收；线程 joinable 且无人 join → 析构即 `std::terminate`。同 T1-XTP-02 | 单跑 rc=134（SIGABRT，`libc++abi: terminating`），断线前无任何断言输出 |

## 留白面（本 lane 没能覆盖的，含原因与行数量级）

| 段 | 行数量级 | 原因 |
|---|---|---|
| `init()` L94-135 | ~39 行（含 L110 standardisePath、L116-117 create_directories、L119-131 dlopen/get_symbol/CreateQuoteApi、L132 RegisterSpi） | 本机无 macOS 厂商库，L131 必 NULL-call 崩；`m_funcCreator` 由 `get_symbol` 现场覆写，**镜像缝也救不了这段**（预置的 creator 会在 L130 被 NULL 冲掉）。只能等主控提供「可注入的 loader 缝」或产品侧加判空。见 T1-XTP-01 |
| `connect()` 的建线程块 L146-158 | ~12 行 + 1 个 missed function（worker lambda L150-157） | `while(true)` 无停止条件、`_asyncio` 随对象析构变悬垂 → 挂死/退出期崩。见 T1-XTP-02 |
| `OnDepthMarketData` 的 union 扩展字段（股票/期权/债券附加数据）| 0 行（产品根本没读） | 不是留白，是产品未使用 `XTPMD` 的 `stk/opt/bond` union，无对应换算可测 |
| `XTPMD/XTPST.ticker` 定长数组的「满格无 `\0`」输入 | 不可测面 | 属已登记的越界读缺陷族（`bugs/R16_share_tools.md` 同族）。本轮规则明确「夹具不要新造越界输入去证明崩溃」，只在 `bugs/T27_parserxtp.md` 的 BUG-T27-009 里登记嫌疑位置，不构造触发 |
| `strToTime` 的 O(n²) `strlen` 循环、`checkValid` 只认 DBL_MAX/FLT_MAX 两个精确值 | 无新增行 | 行为已由 T1 的 `time_and_price_helpers_*` 钉住；负价/NaN 的语义问题写进 BUG-T27-001 的备注，不单开条目 |

## 门禁实测（`docs/迭代计划/迭代01/tools/case_runner.sh`，绝对路径 bin，运行目录每轮新建）

- 构建：`cmake --build build-cov --target CoverageTests_t_ParserXTP` → 退出码 0（`-Wall -Wextra -Werror` 原样，未放宽）
- 枚举 62 条（`grep -v '\.DISABLED_'` 后），per-case `-P 4`、CASE_TIMEOUT 60：
  **PASS 59 / FAIL 2 / CRASH 0 / HANG 0 / NORUN 1**
- 2 条 FAIL 与 1 条 NORUN 都**不是本 lane 的用例**，且在本人加文件之前的基线里就是这样（基线 `/tmp/t27base`：12 PASS / 2 FAIL）：
  - `ParserXTP.depth_market_data_converts_snapshot_and_level1_queues`（T1 的夹具算错：`md.bid[9]=12.0+9` 是 21.0，断言写的 12.9）
  - `ParserXTP.time_and_price_helpers_strip_colons_and_guard_sentinels`（T1 期望 `strToTime("14:59:59.999")==145959999`，实际 1459588999 —— 产品的 `.` 没剥、`convert::to_uint32` 不校验数字，见 BUG-T27-001）
  - `DISABLED_ParserXTP.init_and_connect_require_vendor_lib_and_leak_thread` → NORUN：该 lane 把 `DISABLED_` 挂在 **suite 名**上，`grep -v '\.DISABLED_'` 滤不掉，于是被枚举出来又一条用例都不跑。**本 lane 的 3 条 DISABLED 一律挂在用例名上**，可被正常滤掉。
- 稳定性：同一 cases 列表（62 条）连跑 3 轮、每轮新建 `$T/run`，59/2/1 的判决集三轮字节一致（`md5 *.rc|sort|md5` 相同），无抖动。
- 覆盖率（只报本 target 的绝对增量，不看百分比口径）：`src/ParserXTP/ParserXTP.cpp`
  lines 550 / missed **266 → 45**；regions 52.85% → 95.82%；functions missed **9 → 2**；branches missed **111 → 36**。
  剩余 missed 行 = `95-120`、`128-135`（init）与 `147-158`（connect 建线程块），与上表留白面一致。
