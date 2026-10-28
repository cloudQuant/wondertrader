# lane T4 manifest —— TraderFemas / CTPOptLoader / ParserCTPMini

工作流: 测试覆盖率迭代 01 / lane T4（第六波·收发族）。
范围: `src/TraderFemas`、`src/CTPOptLoader`、`src/ParserCTPMini`。
`src/**` 生产代码零改动；缺陷一律记在 `bugs/T4_femas_ctploader_mini.md`（19 条，全部 `待审核`）。

门禁（唯一合法口径，本轮全部实测 exit 0、零告警）：

```
sh docs/迭代计划/迭代01/tools/gate_flags/t_TraderFemas.sh      src/TestUnits/coverage/t_TraderFemas/femas_trader_test.cpp
sh docs/迭代计划/迭代01/tools/gate_flags/t_CTPOptLoader.sh      src/TestUnits/coverage/t_CTPOptLoader/ctpopt_loader_test.cpp
sh docs/迭代计划/迭代01/tools/gate_flags/t_ParserCTPMini.sh     src/TestUnits/coverage/t_ParserCTPMini/ctpmini_parser_test.cpp
```

## 新增文件（数字取自磁盘）

| 文件 | 行数 | 用例总数 | 启用 | DISABLED_ | EXPECT/ASSERT 数 |
|---|---|---|---|---|---|
| `t_TraderFemas/femas_trader_test.cpp` | 1134 | 39 | 35 | 4 | 247 |
| `t_CTPOptLoader/ctpopt_loader_test.cpp` | 531 | 19 | 16 | 3 | 111 |
| `t_ParserCTPMini/ctpmini_parser_test.cpp` | 469 | 17 | 14 | 3 | 87 |
| 合计 | 2134 | 75 | 65 | 10 | 445 |

说明两点：
1. **TraderFemas 启用了 35 条，超出「每模块 8~15 条」的带宽**。原因是该模块的主体价值就在
   换算矩阵（8 个 wrap* × 正反双向 + 4 个 make* 转换），把它们合并成 3 条大用例更省数量但会丢掉失败定位能力。
   需要收紧的话，`wrap_*` 4 条与 `make_*` 4 条可以合并成 2 条，其余 27 条不建议动。
2. 三个目录的 `gate_placeholder_test.cpp` 都保持原样，没有新增/删除用例。

## 形状要点（与主控给的构建事实对齐）

- 一个模块目录 = 一个 `CoverageTests_t_<模块>` 目标：生产 TU 与用例同二进制，
  所以用例直接 `new TraderFemas()` / `CTraderSpi spi;` / `createParser()`，不需要 dlopen。
- **没有用 `#define private public`**：
  - `TraderFemas` 的成员是 `protected` → 用派生探针 `FemasProbe`（仅 `using` 暴露既有成员 + 读写既有字段，不改行为）；
  - `ParserCTPMini` 成员是 `private` → 不派生，只打它 public 的 SPI/ITraderApi 表面；
  - `CTPOptLoader` 的 `CTraderSpi` 只实例化，私有方法改由同 TU 里同名的全局符号（`run`、`extractProductID`、
    `pUserApi`、`_commodities`、`FRONT_ADDR` 等）间接驱动与断言。
- **没有真的 dlopen**：`run()` 的 5 条启用用例全部靠 `CTPOptLoader.cpp:198-201` 的必填项校验
  （SAVEPATH 留空）在 `:238` 的 `load_library` 之前返回；TraderFemas/ParserCTPMini 的 `init()` 整条进 DISABLED。
- **不起线程/不 sleep/不连柜台**：`TraderFemas::triggerQuery` 需要的 `m_strandIO` 由 fixture 用测试自己的
  `boost::asio::io_context` + `strand` 顶上，只 post 不 run，队列 handler 永不执行（因此哨兵 API 指针不会被解引用）。

## 用例 → 生产文件:行段

### `t_TraderFemas/femas_trader_test.cpp` → `src/TraderFemas/TraderFemas.cpp`（1180 行）

| 用例（行号=用例内定义行） | 生产行段 | 证伪点 |
|---|---|---|
| `fresh_instance_reports_disconnected_and_refuses_every_business_call` :344 | :71-93 构造、:1139-1142 isConnected、:325-330 orderInsert 守卫、:394-397 orderAction 守卫、:424-429/:446-451/:466-471/:487-492 四个查询守卫、:305-310 logout | 6 个 -1 与 isConnected=false，守卫顺序一变即红 |
| `login_without_api_handles_credentials_but_still_fails` :368 | :288-303（:290-292 赋值在 :294 空指针检查之前） | 返回 -1 但 m_strUser/Pass/Product 已被写入 |
| `ready_state_flips_connectivity_flag` :380 | :1139-1142、WrapperState 枚举 `TraderFemas.h:48-57` | 只有 WS_ALLREADY 算已连接 |
| `release_marks_stopped_and_is_repeatable` :389 | :140-167（:142 m_bStopped、:144/:151/:156-166 四个空分支） | 幂等 + m_bStopped 变 true |
| `register_spi_binds_sink_and_base_data_manager` :397 | :241-248、:508-512、:538-541、:35（write_log 的 sink==NULL 早退） | bdMgr 绑定地址相等 + WTE_Connect 事件 |
| `front_disconnected_resets_state_and_publishes_close_event` :413 | :525-531 | WTE_Close/44、状态回落、m_bQryOnline 归 false |
| `qry_front_disconnected_only_clears_query_flag` :426 | :533-536 | 只动 m_bQryOnline，不动交易通道状态 |
| `heartbeat_warning_is_routed_to_sink_log` :438 | :538-541 | LL_DEBUG + "Heartbeating" 文案 |
| `make_entrust_id_rejects_null_and_zero_length_buffer` :452 | :221-224 | 两个守卫都不写缓冲 |
| `make_entrust_id_pads_local_order_id_to_twelve_digits` :463 | :226-231、:255-258 | 首号 "0000000000002"、次号 "...003" |
| `make_entrust_id_prefixes_session_id` :476 | :230、:250-253 | "A1000000000002"；genRequestID 从 1 递增 |
| `wrap_wts_enums_into_femas_codes` :493 | :904-914、:842-854、:878-890、:926-934、:954-960 | 4×价格(含 CFFEX 分叉)/4×方向/5×开平/3×时效/2×动作 共 18 个映射值 |
| `wrap_femas_codes_back_into_wts_enums` :522 | :916-924、:856-868、:870-876、:892-902、:936-944、:1144-1150 | 反查 15 个映射 + wrapExchg 的 CZCE→ZCE 改名 |
| `wrap_order_state_clamps_femas_states_above_accepted_no_reply` :553 | :946-952 | 阈值上/下两套行为 |
| `is_error_rsp_info_and_make_error` :562 | :1115-1121、:1050-1057 | NULL/ErrorID=0/非 0 三态 + WTSError 码与文案 |
| `make_order_info_maps_vendor_order_fields` :585 | :963-1013（:995-999 日期时间、:1000 状态、:1002-1005 委托号） | 13 个字段，含 "09:30:15"→93015→`makeTime(...,93015000)` |
| `make_order_info_falls_back_to_local_id_and_maps_fok` :630 | :965-967 空合约、:977-987 orderFlag 三分支、:1002-1005 | atoi("0")==0 回落 OrderLocalID；IOC+CV→FOK |
| `make_entrust_maps_vendor_input_order` :664 | :1015-1048 | 10 个字段 + 空合约返回 NULL |
| `make_trade_record_scales_amount_by_contract_volume_multiple` :697 | :1059-1113（:1103 金额、:1073-1076 时间） | amount=volScale×vol×price=71000；空合约返回 NULL |
| `certification_rejection_fails_login_without_touching_api` :737 | :551-565 | WS_LOGINFAILED + onLoginResult(false,"authcode expired") |
| `login_rejection_fails_login_without_touching_api` :751 | :567-595 的 else 支（:587-594） | 同上，不碰 m_pUserAPI |
| `qry_login_completes_initialization_when_already_logined` :763 | :543-549 → :804-810 | 通道就绪翻转 + onLoginResult(true) |
| `qry_login_before_user_login_does_not_open_channel` :780 | :543-549 的 false 支 | 只置 m_bQryOnline |
| `logout_rsp_returns_channel_to_not_login` :791 | :597-602 | WTE_Logout + 状态回落 |
| `order_action_rejection_pushes_cancel_error` :802 | :620-632 | WEC_ORDERCANCEL 错误 + 成功应答不产生错误 |
| `order_insert_response_carries_error_and_entrust` :817 | :604-618、:1015-1048、:1050-1057 | 空合约整条丢弃；有合约时 err 透传（含 err==NULL） |
| `queries_are_enqueued_only_after_channel_is_ready` :853 | :424-444、:446-506、:1152-1180（post 侧） | 4 次入队 size==4，handler 未执行 |
| `order_insert_reaches_contract_lookup_before_using_api` :872 | :327-344 | 过了状态守卫后在合约缺失处返回 -1 |
| `account_query_response_computes_balance` :899 | :634-665（:655 balance 公式、:636-640 bIsLast） | 非末包不落地；106300 = 100000+5000-300+2000-400 |
| `position_query_response_never_appends_the_item_it_builds` :928 | :667-742（缺陷 BUG-T4-002） | size==0 的哨兵断言，修复后会红 |
| `trade_query_response_reports_appended_records` :951 | :744-772 | 错误末包 size 0 / 正常末包 size 1 |
| `order_query_and_push_callbacks` :979 | :774-802、:818-828、:830-840 | 查询数组 size 1 + 两路推送字段 |
| `instrument_status_push_casts_vendor_status` :1021 | :1133-1137 | exchg/code/状态直投；无 sink 静默 |
| `error_rsp_is_written_to_log_with_request_id` :1043 | :812-816、:1123-1126、:1128-1131、:514-523 | 日志含 request id；三个空实现无副作用 |
| `set_and_get_tag` :1060 | `TraderFemas.h:75-76` | setTag/getTag 往返 |
| `TraderFemasTest.DISABLED_user_login_success_branch_needs_vendor_api` :1078 | :567-586（:578 空指针） | BUG-T4-004 |
| `TraderFemasDisabled.DISABLED_query_account_before_connect_dereferences_null_strand` :1096 | :441 → :1152-1154 | BUG-T4-003 |
| `TraderFemasDisabled.DISABLED_make_entrust_id_ignores_buffer_capacity` :1110 | :221-239 | BUG-T4-005 |
| `TraderFemasDisabled.DISABLED_init_dlopens_vendor_module` :1125 | :95-138（:125 load_library） | BUG-T4-007 |

### `t_CTPOptLoader/ctpopt_loader_test.cpp` → `src/CTPOptLoader/CTPOptLoader.cpp`（268 行）+ `TraderSpi.cpp`（623 行）

| 用例 | 生产行段 | 证伪点 |
|---|---|---|
| `yaml_content_fills_globals_and_stops_before_module_load` :122 | CTPOptLoader.cpp:75-116（isFile=false 支）、:198-201 必填校验、:238 之前返回 | 12 个全局（含 commfile/contfile/module 三个内建默认）+ `pUserApi` 仍为 NULL（没 dlopen 的硬证据） |
| `yaml_content_honours_configured_names_and_flags` :159 | :93-113（commfile/contfile/mapfiles/onlyincfg/incremental/module） | mask=7、mycomm.json、./myoptapi.so、两个 bool 变 true |
| `empty_content_returns_zero_without_touching_globals` :186 | :77-79（load_from_content 返回 NULL） | 哨兵全局不被触碰 |
| `missing_json_file_returns_zero_without_touching_globals` :198 | :146-150 + WTSCfgLoader.cpp:298-306 | 文件不存在 → root NULL → 早退 |
| `ini_branch_applies_builtin_defaults_for_absent_file` :207 | :117-145（IniHelper::load 吞异常，IniHelper.hpp:73-86） | ini 支 mask 默认 7 ≠ yaml 支默认 0；两个 bool 默认 false |
| `extract_product_id_stops_at_first_digit` :230 | TraderSpi.cpp:74-85 | 5 个代码切分（BUG-T4-019 的现行为被钉住） |
| `extract_product_name_strips_trailing_digits` :241 | TraderSpi.cpp:87-98 | 只剥尾部数字 |
| `futures_instrument_builds_commodity_and_contract` :255 | TraderSpi.cpp:228-390（:231-247 授权判定、:249-283 名称、:300-317 合约字段、:327-382 品种字段） | 品种 10 项（SHFE→平今/限价、precision 0）+ 合约 8 项 |
| `non_shfe_future_uses_open_cover_and_both_price_modes` :298 | TraderSpi.cpp:346-375 | 非 SHFE/INE 走开平+双价格模式；PriceTick 0.5 → precision 1 |
| `option_instrument_keeps_contract_level_volume_multiple` :317 | TraderSpi.cpp:310-315、:335-360 | 期权 volScale 兜底 1、OT/underlying/strike 落合约 |
| `class_mask_filters_ungranted_categories` :350 | TraderSpi.cpp:235-245 | mask=2 丢期货、mask=1 丢期权、Combination 永不被授权 |
| `onlyincfg_drops_instruments_without_a_name_mapping` :377 | TraderSpi.cpp:258-263 | ONLYINCFG 的 break；补映射后放行 |
| `mapped_product_name_rebuilds_contract_name` :396 | TraderSpi.cpp:249-256、:272-280、:334 | MAP_NAME/MAP_SESSION 命中支：cname=品种名+月份后缀 |
| `error_packet_and_null_instrument_collect_nothing` :416 | TraderSpi.cpp:229-231、:384-386 | 错误应答与空指针包都不落地 |
| `authenticate_rejection_does_not_fire_login_request` :434 | TraderSpi.cpp:131-139 | 错误应答不发登录请求（iRequestID 不变） |
| `login_rejection_and_error_packet_are_side_effect_free` :449 | TraderSpi.cpp:151-170 的失败支、:610-614、:603-607、:616-623 | 拒绝登录不推进；OnRspError/OnFrontDisconnected 无副作用 |
| `CTPOptLoaderTest.DISABLED_last_instrument_packet_exits_the_process` :481 | TraderSpi.cpp:391-394（`exit(0)`） | BUG-T4-015 |
| `CTPOptLoaderDisabled.DISABLED_complete_config_reaches_dlopen_and_null_creator` :499 | CTPOptLoader.cpp:238-265 | BUG-T4-014 |
| `CTPOptLoaderDisabled.DISABLED_front_connected_and_login_success_require_vendor_api` :520 | TraderSpi.cpp:111-116、:118-130、:151-170 | 同一 BUG-T4-014 家族的 NULL `pUserApi` |

### `t_ParserCTPMini/ctpmini_parser_test.cpp` → `src/ParserCTPMini/ParserCTPMini.cpp`（467 行）

| 用例 | 生产行段 | 证伪点 |
|---|---|---|
| `create_and_delete_round_trip` :165 | :40-55（extern "C" createParser/deleteParser） | 工厂类型可 downcast、isConnected=false、deleteParser 置空入参 + 二次调用安全 |
| `lifecycle_calls_without_api_are_safe` :181 | :141-163 connect/disconnect/release、:456-459 isConnected | 三个 API 守卫都在解引用之前 |
| `strips_colons_and_keeps_digits` :197 | :62-74 全局 strToTime | 5 组时间串（含空串→0） |
| `depth_market_data_is_converted_into_a_tick` :210 | :213-330（:218-221 日期时间、:255-260 合约、:262-296 字段、:327-329 handleQuote） | 24 个 tick 字段，含 93015×1000+250、五档价量、exchg 取品种而非行情包 |
| `invalid_prices_become_zero` :249 | :77-82 checkValid、:266-269 | DBL_MAX/FLT_MAX → 0 |
| `czce_turnover_is_scaled_by_volume_multiple` :267 | :281-285 | CZCE 分支 turnover×volScale=15000 |
| `missing_action_day_falls_back_to_trading_date` :294 | :222-224、:226-252（走小时<20 的确定支） | ActionDay="" → 用 m_uTradingDate 兜底 |
| `unknown_contract_drops_the_tick` :307 | :255-260 | 合约查不到直接丢笔 |
| `quote_is_dropped_when_sink_unregistered` :318 | :327-329 | 换算照常、投递被守卫拦下 |
| `front_connected_publishes_event_and_survives_missing_api` :333 | :171-180、:350-371（ReqUserLogin 的 NULL 守卫） | WPE_Connect + 日志；不发登录请求 |
| `login_response_is_ignored_until_last_packet` :346 | :178-193 的 false 支、:410-413 IsErrorRspInfo 桩 | 只有 bIsLast=false 才走得通（BUG-T4-010 的直接后果） |
| `logout_and_close_events` :361 | :201-206、:195-200 | WPE_Logout / WPE_Close + LL_ERROR 日志 |
| `heartbeat_and_sub_response_paths_log_only` :378 | :343-348、:333-341、:165-168 | 心跳文案带 elapse；订阅/错误/退订回调不产生事件 |
| `subscribe_before_login_only_caches_the_filter` :400 | :415-451（m_uTradingDate==0 支）、:453-455 unsubscribe | 未登录只缓存，不写 500 槽栈数组（BUG-T4-013） |
| `ParserCTPMiniDisabled.DISABLED_login_success_branch_dereferences_null_api` :426 | :178-193（:183 空指针） | BUG-T4-010 + BUG-T4-009 |
| `ParserCTPMiniDisabled.DISABLED_callback_before_register_spi_uses_wild_sink` :448 | :83-88 构造（m_sink/m_pBaseDataMgr 未初始化） | BUG-T4-008 |
| `ParserCTPMiniDisabled.DISABLED_init_dlopens_vendor_module` :458 | :96-135（:126-131） | BUG-T4-009 |

## 本轮没有进入的模块面（覆盖率缺口，勿当成「已验证」）

| 缺口 | 位置 | 为什么进不去 |
|---|---|---|
| TraderFemas 登录成功/认证请求/下单撤单正路径 | :260-286、:325-422 的厂商调用段、:567-586 | 需要真实 `CUstpFtdcTraderApi` 实例（47 个纯虚 + 仓库无该 .dylib）；本轮按约束不 dlopen |
| TraderFemas init/connect/disconnect 与 worker 线程 | :95-138、:169-219 | dlopen + 起线程 + 50ms sleep（规则 6） |
| TraderFemas 夜盘/结算类期权接口 | 无（该模块不含） | — |
| CTPOptLoader DumpToJson / LoadFromJson | TraderSpi.cpp:397-486、:488-608 | 最后一包 `exit(0)`（BUG-T4-015）与 NULL creator（BUG-T4-014）双重阻塞 |
| CTPOptLoader 的 mapfiles 解析 | CTPOptLoader.cpp:205-235 | 在必填项校验之后，走不到 |
| ParserCTPMini init/connect/subscribe 登录后的真订阅 | :96-135、:415-451 的第二支 | dlopen + 空 creator 调用；`m_uTradingDate` 只能由会崩的登录成功分支置非 0 |
| ParserCTPMini 夜盘日期纠偏 | :228-252 | 依赖 `TimeUtils::getDateTime` 挂钟（BUG-T4-012），无确定性断言可写 |
| ParserCTPMini `m_pBaseDataMgr==NULL` 早退支 | :141-144 | 构造函数没初始化该指针（BUG-T4-008），"早退"与"崩"取决于栈残值，不可证伪 |
