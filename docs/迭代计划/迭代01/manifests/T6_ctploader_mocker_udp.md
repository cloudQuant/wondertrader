# T6 manifest — 收发族第六波（CTPLoader / TraderMocker / ParserUDP）

- 波次: 迭代01 第六波 · lane T6
- 用例目录: `src/TestUnits/coverage/t_CTPLoader/`、`src/TestUnits/coverage/t_TraderMocker/`、`src/TestUnits/coverage/t_ParserUDP/`
- API 简报依据: `manifests/api-brief-T01.md`（CTPLoader）、`api-brief-T06.md`（TraderMocker）、`api-brief-T03.md`（ParserUDP）
- 门禁实测（唯一合法口径，均 **exit 0**，含 `-Wall -Wextra -Werror`）:
  - `sh docs/迭代计划/迭代01/tools/gate_flags/t_CTPLoader.sh src/TestUnits/coverage/t_CTPLoader/t6_ctploader_reachable_test.cpp`
  - `sh docs/迭代计划/迭代01/tools/gate_flags/t_TraderMocker.sh src/TestUnits/coverage/t_TraderMocker/t6_tradermocker_reachable_test.cpp`
  - `sh docs/迭代计划/迭代01/tools/gate_flags/t_ParserUDP.sh src/TestUnits/coverage/t_ParserUDP/t6_parserudp_reachable_test.cpp`
- 未改动: 任何 `src/**` 生产代码、任何 `CMakeLists.txt`、`t_TraderMocker/seam_tradermocker_test.cpp`（别人的探针）、
  三个目录各自的 `gate_placeholder_test.cpp`。
- 用例合计: **42 条启用 + 10 条 `DISABLED_`**（每模块 12–15 条启用，落在 8~15 区间）。
- 缺陷: 11 条，全部 `- 状态: 待审核` → `docs/迭代计划/迭代01/bugs/T6_ctploader_mocker_udp.md`

形状约束复核（本轮实测确认，不是转述）：三个目录各自定义同名 `extern "C"` 工厂
（`createTrader/deleteTrader` 与 `createParser/deleteParser`；CTPLoader 则是 `extern "C" run`），
所以必须一目录一目标 `CoverageTests_t_<模块>`，本 lane 的三个文件因此分处三个目录、互不合链。

---

## 一、`src/TestUnits/coverage/t_CTPLoader/t6_ctploader_reachable_test.cpp`

15 启用 + 5 `DISABLED_`。可观察通道只有两个：`run()` 的返回码，以及把 `std::cerr` 重定向进内存
（胶水层所有回报都走 `std::cerr`，因此「有没有打印某一行」就是可证伪断言）。

| 用例 | 生产文件:行段 | 断言的可证伪点 |
| --- | --- | --- |
| `t6_ctploader_run.empty_inline_content_is_rejected_before_any_loading` | `CTPLoader.cpp` L76、L84-88（`isFile=false` → `load_from_content` → NULL） | 返回 0 且 cerr 全空（一旦往下走厂商装载必打日志） |
| `t6_ctploader_run.inline_yaml_without_savepath_stops_at_required_field_guard` | `CTPLoader.cpp` L84-122 全段（`ctp`/`config` 读取、L113-120 module 缺省名兜底）+ L196-204 必填守卫 | 返回 0；SAVEPATH 空 ⇒ 守卫必须在 `CreateFtdcTraderApi` 之前命中 |
| `t6_ctploader_run.unparsable_inline_yaml_escapes_through_the_c_entry` | `CTPLoader.cpp` L86 + `WTSCfgLoader.cpp` L233-247（`YAML::Load` 抛出点） | 实测异常穿透 `extern "C"`（`threw==true`）；若将来被 catch 或防护成返回值，本断言同样会红（BUG-T6-004） |
| `t6_ctploader_run.missing_config_files_are_rejected_for_both_layouts` | `CTPLoader.cpp` L156-160（JSON 文件不存在）+ L124-155（`.ini` 不存在，`IniHelper::load` L73-86 吞异常）+ L196-199 | 两条路径都返回 0 且不打日志 |
| `t6_ctploader_run.json_file_with_ctp_and_config_stops_at_savepath_guard` | `CTPLoader.cpp` L156-194（含 L172 `mask`、L179-181 bool 项、L184-192 module 兜底）+ L201-204 | config 段无 `path` ⇒ 必须返回 0 |
| `t6_ctploader_run.ini_with_complete_ctp_but_no_savepath_is_rejected` | `CTPLoader.cpp` L124-155 全部 ini 读取（L138 `mask`、L139-141 `onlyincfg/incremental/qryfees` 的 true 侧、L142 `feefilter`、L144-148 三个文件名 + mapfiles） | 五字段齐全仍因 `path` 缺失返回 0 |
| `t6_ctploader_run.ini_with_empty_front_is_rejected_although_savepath_present` | `CTPLoader.cpp` L201（`FRONT_ADDR.empty()` 那一侧） | 反向组合，证明守卫不是只看 SAVEPATH |
| `t6_ctp_loader_names.extract_product_id_stops_at_first_non_letter` | `TraderSpi.cpp` L85-96 | `"ag2512"→"ag"`、`"IF2509"→"IF"`、`"2512"→""`、`"m"→"m"`、`"rb2510"→"rb"` |
| `t6_ctp_loader_names.extract_product_name_trims_trailing_month_digits` | `TraderSpi.cpp` L98-109 | `"Silver2512"→"Silver"`、`"ab1"→"ab"`、`"a1x"→"a1x"`、`"2512"→"2"`（`idx>0` 早停） |
| `t6_ctp_loader_spi.front_disconnected_is_reported_on_stderr` | `TraderSpi.cpp` L847-851 | cerr 含 `OnFrontDisconnected` 与原因码 `-5` |
| `t6_ctp_loader_spi.rsp_error_prints_detail_only_for_real_errors` | `TraderSpi.cpp` L854-857 + L859-866（`IsErrorRspInfo`，含 `(pRspInfo)` 短路） | ErrorID=32 打明细；ErrorID=0 与 pRspInfo=NULL 都不打 |
| `t6_ctp_loader_spi.authenticate_responses_never_reach_login_when_guarded` | `TraderSpi.cpp` L138-145 | 错误应答/非 last 都不得触发 `ReqUserLogin`（cerr 无 `Requesting`） |
| `t6_ctp_loader_spi.login_failure_response_is_not_treated_as_success` | `TraderSpi.cpp` L158-177 守卫侧 | 错误应答不得写 `FRONT_ID/ORDER_REF`、不得解引用 `pUserApi` |
| `t6_ctp_loader_spi.instrument_responses_skip_body_and_never_reach_exit` | `TraderSpi.cpp` L343-348 两层守卫 + L542-548（`exit(0)` 出口，bIsLast=false 挡住） | 错误应答无 `already exists/Name confirmed`；NULL 载荷时 cerr 全空 |
| `t6_ctp_loader_spi.fee_and_margin_error_responses_are_skipped_before_deref` | `TraderSpi.cpp` L550-556、L578-582（含 L572-575 / L588-591 的 `_queries.empty() && QRYFEES` 守卫） | 只打 `ErrorID=404`，不进入指针解引用与 `exit(0)` |
| `DISABLED_t6_ctploader_spi_crash.on_front_connected_dereferences_null_user_api` | `TraderSpi.cpp` L118-122 → L125-136（`pUserApi->ReqAuthenticate`） | 必崩：`pUserApi` 为 `CTPLoader.cpp` L27 全局，未 run 前恒 NULL |
| `DISABLED_t6_ctploader_spi_exit.instrument_last_response_calls_exit_zero` | `TraderSpi.cpp` L542-548 | `exit(0)` 会杀掉测试进程（连带丢 profraw）→ BUG-T6-002 |
| `DISABLED_t6_ctploader_run_crash.inline_config_without_ctp_section_dereferences_null` | `CTPLoader.cpp` L90-91 / L162-163 | 缺 `ctp` 段时空指针 → BUG-T6-003 |
| `DISABLED_t6_ctploader_run_net.complete_config_reaches_vendor_api_init` | `CTPLoader.cpp` L206-254、L256-288 | 必填项齐全即调用厂商 `CreateFtdcTraderApi`+`Init`(+`Join`)，起线程连前置 → 门禁禁止 |
| `DISABLED_t6_ctp_loader_names_crash.extract_product_name_on_empty_string_reads_out_of_bounds` | `TraderSpi.cpp` L98-109 | `strlen("")-1` 下溢后越界读 → BUG-T6-005 |

## 二、`src/TestUnits/coverage/t_TraderMocker/t6_tradermocker_reachable_test.cpp`

15 启用 + 3 `DISABLED_`。与同目录已跑通的 `seam_tradermocker_test.cpp` 并存（未改动它）。

| 用例 | 生产文件:行段 | 断言的可证伪点 |
| --- | --- | --- |
| `t6_tradermocker_factory.create_delete_roundtrip_clears_caller_pointer` | `TraderMocker.cpp` L34-50（`createTrader`/`deleteTrader` 含 NULL 分支 L44）、L81-95（构造）、L98-108（析构）、L718-721（`isConnected`）、L640-657（`release`） | `deleteTrader` 必须把出参置空；新建实例 `isConnected()==false` |
| `t6_split_volume.volume_at_or_below_min_returns_single_whole_part` | `TraderMocker.cpp` L52-59 | `vol<=minQty` 原样单片返回（3/5→{3}、5/5→{5}、0→{0}） |
| `t6_split_volume.parts_sum_back_to_volume_and_stay_inside_bounds` | `TraderMocker.cpp` L60-79（含 L66-69 上界 clamp、L71-72 的 0 片 continue） | 拆量守恒 `Σ==1000`、每片 ∈[1,100] 且非 0、片数 ≥10 |
| `t6_split_volume.tail_part_is_clamped_and_uniform_when_max_equals_min` | `TraderMocker.cpp` L64-76（`length==1` 的确定性分支） | 尾片 clamp（737 守恒）；`max==min` 时 12/(4,4) 恒为 3 片 4 |
| `t6_make_entrust_id.rejects_null_buffer_and_zero_length` | `TraderMocker.cpp` L120-123 | 两个空值防护都返回 false 且缓冲不被改写 |
| `t6_make_entrust_id.writes_mocker_scoped_sequence_after_init` | `TraderMocker.cpp` L125-129（`format_to` + 后置 `++`） | 前缀 `me.`、含 `.77.`、两条 ID 不同且尾号严格递增 |
| `t6_init.explicit_config_creates_mocker_dir_and_keeps_given_id` | `TraderMocker.cpp` L518-530、L536-546 | init 返回 true 且 `mocker_9311/` 目录出现（`_pos_file` 的落盘点） |
| `t6_init.missing_keys_fall_back_to_qty_defaults_and_zero_id` | `TraderMocker.cpp` L520-526（全取默认）、L531-535（`maxqty 0→100`、`minqty 0→1`）、L538-544 | 空对象配置仍返回 true；`mocker_0/` 建出；委托号里出现 `.0.` |
| `t6_init.init_is_repeatable_and_later_config_wins` | `TraderMocker.cpp` L518-526（第二次覆盖 `_mocker_id`） | 连续两次 init 后委托号带第二次 id（`.9313.`） |
| `t6_lifecycle.register_spi_asks_for_base_data_mgr_once` | `TraderMocker.cpp` L659-664 | `registerSpi` 必须立刻取一次 BDMgr（计数 ==1） |
| `t6_lifecycle.release_and_disconnect_are_idempotent_and_emit_no_events` | `TraderMocker.cpp` L640-657（两次 release 命中 L642-643 早退）、L705-716（disconnect 两侧 + `_thrd_match` 空不 join）、L718-721 | 全程 `isConnected()==false`、`handleEvent` 计数 0 |
| `t6_lifecycle.logout_reports_zero_and_emits_no_replies` | `TraderMocker.cpp` L745-748 | 返回 0 且无 `onLoginResult/handleEvent` |
| `t6_deferred_submit.query_apis_only_enqueue_and_report_nothing` | `TraderMocker.cpp` L830-832+L859-860、L862-864+L908-909、L911-914+L920-921、L923-926+L932-933（四个 `post` 外壳） | 四个查询都返回 0，但 account/position/orders/trades 回报计数必须全为 0（证明回报体在未被 run 的 io_context 上） |
| `t6_deferred_submit.order_insert_with_null_entrust_is_rejected_before_enqueue` | `TraderMocker.cpp` L138-143 | 空委托返回 0 且不产生任何回报/日志（`n_log_==0`） |
| `t6_deferred_submit.valid_insert_and_cancel_requests_are_deferred_not_reported` | `TraderMocker.cpp` L145-146+L325-326（orderInsert 提交侧）、L750-755+L827-828（orderAction 提交侧） | 提交返回 0；`onRspEntrust/onPushOrder/onPushTrade/onTraderError/onRspOrders` 计数全 0 |
| `DISABLED_t6_thread_paths.login_spawns_match_thread_and_sleeps` | `TraderMocker.cpp` L723-743（含 L725-733 撮合线程 + `sleep_for(5ms)`、L735-740 post） | 起线程/睡 5ms → 门禁禁止（规则 6） |
| `DISABLED_t6_thread_paths.connect_binds_real_udp_socket_and_runs_io_context` | `TraderMocker.cpp` L689-703 + L666-687（`open/bind/async_receive_from`） | 真绑 UDP 端口并起 io 线程 → 门禁禁止 |
| `DISABLED_t6_hang_paths.split_volume_with_zero_bounds_spins_forever` | `TraderMocker.cpp` L52-80（`length==1` 且 `curVol` 恒 0 → `while(left>0)` 永不退出） | 挂死路径，只记录不执行（生产被 init L531-535 兜底挡住） |

## 三、`src/TestUnits/coverage/t_ParserUDP/t6_parserudp_reachable_test.cpp`

12 启用 + 2 `DISABLED_`。

| 用例 | 生产文件:行段 | 断言的可证伪点 |
| --- | --- | --- |
| `t6_parserudp_factory.createParser_returns_api_and_delete_clears_pointer` | `ParserUDP.cpp` L62-78、L82-91（构造）、L94-96（空析构）、L283-286（`isConnected`）、L115-118（空 `release`） | 工厂返回可用对象、`deleteParser` 置空出参、未连接 |
| `t6_parserudp_factory.deleteParser_accepts_null_without_touching_anything` | `ParserUDP.cpp` L70-77 | NULL 入参不崩、出参仍为 NULL |
| `t6_parserudp_init.valid_multicast_host_without_gpsize_falls_back_to_default` | `ParserUDP.cpp` L98-113（含 L104-105 `gpsize 0→1000`、L107-111 两个端点） | init 返回 true 且 `isConnected()` 仍 false（init 不开 socket） |
| `t6_parserudp_init.explicit_gpsize_and_unicast_host_accepted` | `ParserUDP.cpp` L103-104、L107-110 | 非 0 gpsize 走「不兜底」一侧 |
| `t6_parserudp_init.non_numeric_host_throws_uncaught_system_error` | `ParserUDP.cpp` L107（`ip::make_address` 抛出点） | `EXPECT_THROW(..., boost::system::system_error)`：init 无 try/catch（BUG-T6-008） |
| `t6_parserudp_init.missing_host_config_throws_instead_of_reporting_failure` | `ParserUDP.cpp` L100、L107 | 缺 `host` 时 `_hots==""` → 抛异常而不是返回 false |
| `t6_parserudp_init.init_can_be_reapplied_with_a_different_host` | `ParserUDP.cpp` L98-112（第二次覆盖端点） | 两次 init 都返回 true，非幂等空函数 |
| `t6_parserudp_spi.first_register_is_silent_and_second_one_warns` | `ParserUDP.cpp` L307-315 + L19-29（`write_log` 实例化与 `handleParserLog` 调用） | 首个 sink 日志计数 0；替换后新 sink 恰好收到 1 条含 `replaced` 的日志 |
| `t6_parserudp_spi.handle_event_is_not_emitted_without_io_context` | `ParserUDP.cpp` L289-300（subscribe）+ L277-278（`post(_strand, doOnDisconnected)`）+ L452-458 | `disconnect()` 后 `handleEvent` 计数必须为 0（事件被 strand 推迟，`_io_service` 未被 run） |
| `t6_parserudp_subs.subscribe_is_idempotent_and_duplicate_codes_are_dropped` | `ParserUDP.cpp` L289-300（insert 与「已存在」两条分支） | 混合重复/新增集合后 `isConnected()==false`，无 `handleSymbolList/handleQuote` 回调 |
| `t6_parserudp_subs.single_code_subscribe_helper_adds_one_entry` | `ParserUDP.cpp` L289-300（全命中已订阅）、L302-305（空 `unsubscribe`） | 重复订阅 + unsubscribe 后状态不变、无事件 |
| `t6_parserudp_state.release_and_disconnect_without_connect_are_safe` | `ParserUDP.cpp` L115-118、L268-281（`_b_socket==NULL` 跳过 close/delete） | 未连接时 `release/disconnect/disconnect` 都安全，`disconnect()` 恒返回 true |
| `DISABLED_t6_parserudp_net.connect_binds_ports_and_starts_io_thread` | `ParserUDP.cpp` L254-266 + L120-171（`open/bind/async_receive_from`、`new StdThread(io_context::run)`）、L173-214（`subscribe()` 组包）、L216-237（`do_send`） | 真绑端口 + 起线程 → 门禁禁止；这也是报文体解析唯一的生产入口 |
| `DISABLED_t6_parserudp_net.extract_buffer_needs_a_seam_before_it_can_be_fed` | `ParserUDP.cpp` L318-355（`handle_read` 长度校验 L335-338）、L357-441（包头 `_type` 五路分发） | 私有 + `_b_buffer/_s_buffer` 私有 ⇒ 「用内存 buffer 直接喂解析函数」当前不可实现（BUG-T6-006/011） |

---

## 四、本轮未进入的面（诚实口径，供第七波/评审排期）

1. **`TraderMocker.cpp` L328-514（`match_once()` 全文）** —— 撮合价量判定（L355 `price>0`、L381-390 盘口量价为 0 的跳过、
   L394-395 限价与最新价的正/负判定）、持仓增减与冻结量推进（L436-474）、订单状态推进（L424-434 的 `AllTraded/PartTraded_Queuing`）、
   `makeTradeID/makeOrderID`（L110-118）：**全部是私有成员**，且 `_io_service/_positions/_awaits/_ticks` 也是私有，
   公开接口零接缝；本轮又禁止 `#define private public`、禁止起线程 → 一行未进。这是 `TraderMocker.cpp` 984 可执行行里最大的空洞（约 190 行）。
2. **`TraderMocker.cpp` L146-323 的 orderInsert lambda 体**（合约/价格类型/数量/方向/价格合法性、平仓可平量与冻结，
   L160-266 + 回报字段映射 L270-311）与 **L755-825 的 orderAction lambda 体**（撤单释放冻结、`WOS_Canceled` 推进）：
   只有 io_context 被 run 才会执行，而 run 它唯一的路径是 `connect()`/`login()` 的线程。
3. **`TraderMocker.cpp` L549-638（`load_positions/save_positions` 的 JSON 读写与配置正负路径）**：私有 + 只被 lambda 调用。
   `_pos_file` 为空串时 `StdFile::write_file_content` 的行为也未验证（需接缝）。
4. **`TraderMocker.cpp` L967-1006（UDP 包头 + `extract_buffer`）** 与 **`ParserUDP.cpp` L318-441（`handle_read`/`extract_buffer`）**：
   两条 UDP 解析链都需要私有访问 + 真实 socket 回调才能进入；本轮只能以 `DISABLED_` 与缺陷记录（BUG-T6-006/007）留下钩子诉求。
5. **`CTPLoader.cpp` L206-278**（`SET_FILTERS` 过滤、映射文件 `[Name]/[Session]` 读取与 GBK→UTF8 转换、
   `./CTPFlow/` 建流目录、厂商 creator 获取与 `RegisterFront/Init/Join`）：越过 L201 守卫就必然起厂商线程，门禁禁止。
6. **`TraderSpi.cpp` L179-548、L595-846**（`ReqQryInstrument/ReqQryCommission/ReqQryMargin/AppendQuery` 与
   合约→品种/费率映射主体、`DumpToJson/DumpFees/LoadFromJson`）：依赖非空 `pUserApi`、`CLASSMASK/MAP_NAME/ONLYINCFG/QRYFEES`
   全局注入点，且以 `exit(0)` 收尾（BUG-T6-002）。这是 CTPLoader 831 可执行行里的主体（约 700 行未进）。

## 五、给主控的构建侧提示

- 三个文件都**只引用各自模块的生产 TU 符号**（`splitVolume`/`run`/`extractProductID`/`extractProductName`/
  `createTrader`/`createParser`），不跨目录引用，`CoverageTests_t_<模块>` 一目录一目标可独立成立。
- `t_CTPLoader` 目标必须能解析厂商静态库符号（`CThostFtdcTraderApi::CreateFtdcTraderApi` 等，见
  `src/CTPLoader/CMakeLists.txt` L23-32 的 `API/CTP6.6.9/darwin/*.a`）——这是 `CTPLoader.cpp` 自带的引用，
  与本 lane 用例无关；若链接阶段报 undefined symbol，请先按既有口径补 `src/API/...` 的 `.a`，
  本轮用例在运行时**不会**调用它（所有 `run()` 调用都在 L201 守卫返回）。
- 运行副作用：`t6_init.*` 会在工作目录创建并删除 `mocker_<id>/`（`TraderMocker::init` L538-541 的硬编码路径，见 BUG-T6-008），
  `MockerDirGuard` 在用例前后各清一次；`t6_ctploader_run.*` 的临时配置只落 `temp_directory_path()`，析构即删。
- 覆盖率预期（按本轮可直达面粗算，待 `llvm-cov` 实测校正）：`TraderMocker.cpp` 约 100–130/984 行、
  `ParserUDP.cpp` 约 85–105/458 行、`CTPLoader.cpp` 约 60–75/240 行、`TraderSpi.cpp` 约 35–55/831 行。
  剩余主体全部卡在 BUG-T6-011 记的「无接缝」上。
