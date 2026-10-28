# R11 交付清单 — WtUftCore 加深波

- lane: R11（覆盖率迭代 01 第七波）
- 归属目标: `CoverageTests_uftcore`
- 用例目录: `src/TestUnits/coverage/q04_uftcore3/`（前缀 `r11_`，未新建目录、未改 CMakeLists）
- 替身来源: 复用 `q04_trader_fixture.h` 的 `FakeTraderApi` / `FakeBaseDataMgr` / `FakeSink` / `TempRoot` / `policyJsonOpenCloseLimited` / `orderFromSnap` / `posItem`
- 门禁: 真实构建口径 `ninja -t commands ... | tail -1` 取整行换 `-fsyntax-only`，6 个文件全部 exit 0（含 `-Werror`）
- 统计: 6 文件 / 44 条启用 / 1 条 DISABLED

行号均为本轮实测（`grep -n` / `sed -n` 直接取自生产文件）。

---

## 1. `r11_share_manager_test.cpp` → `src/WtUftCore/ShareManager.cpp`（190/190 → 目标面）

| 用例 | 生产文件:行段 | 打到的分支 |
|---|---|---|
| initializeMissingModuleReturnsFalse | ShareManager.cpp:26-36 | `initialize()` 的 `StdFile::exists` 假分支 + warn；重复调用仍 false |
| 〃 | ShareManager.cpp:78-81, 121 | `start_watching()` 的 `!_inited` 早退 |
| 〃 | ShareManager.cpp:126-129 | `init_domain()` 的 `!_inited` 早退 |
| 〃 | ShareManager.cpp:151-154 | `commit_param_watcher()` 的 `!_inited` 早退 |
| setValueRejectedWhenNotInited | ShareManager.cpp:171-174 / 189-192 / 207-210 / 225-228 / 243-246 / 261-264 | `set_value` 六个重载（double/uint64/uint32/int64/int32/string）的 guard |
| getValueFallsBackToDefaultWhenNotInited | ShareManager.cpp:280-283 / 301-304 / 320-323 / 339-342 / 358-361 / 377-380 | `get_value` 六个重载回吐默认值（含缺省 0/""） |
| allocateValueReturnsNullWhenNotInited | ShareManager.cpp:396-401 / 417-422 / 438-443 / 459-464 / 480-485 / 501-506 | `allocate_value` 六个重载返回 nullptr（含 `isExchg=true` 实参组合） |
| selfIsStableSingleton | ShareManager.h:267-271 | `self()` 单例身份 + `set_engine` |

## 2. `r11_parser_adapter_test.cpp` → `src/WtUftCore/ParserAdapter.cpp`

| 用例 | 生产文件:行段 | 打到的分支 |
|---|---|---|
| initRejectsNullConfig | ParserAdapter.cpp:39-42 | `cfg == NULL` → false |
| initRejectsEmptyModule | ParserAdapter.cpp:39-60 | `_cfg->retain()` 后 `module` 为空 → false；二次 init 命中 `_cfg != NULL` |
| initRejectsUnloadableModule | ParserAdapter.cpp:55-72 | 拼路径 + `load_library` 返回 NULL → log_dyn 错误 → false |
| initExtRejectsNullApi | ParserAdapter.cpp:160-164 | `api == NULL` → false |
| initExtSubscribesAllContracts | ParserAdapter.cpp:160-227 | `registerSpi` + `api->init` 真 + `_filters` 空的 else 全量订阅 + `subscribe/clear`；`run()` 243-249；`getBaseDataMgr` |
| initExtStillReturnsTrueWhenApiInitFails | ParserAdapter.cpp:160-227 | `api->init` 假分支（错误日志），仍返回 true |
| releaseToleratesNullApi | ParserAdapter.cpp:229-249 | `release()` 的空 api（`delete nullptr`）+ `run()` 的 `_parser_api == NULL` |
| handleQuoteRejectAndPassThrough | ParserAdapter.cpp:251-265 | `quote==NULL / actiondate==0 / 合约缺失 / 成功改写全码后 stub 透传 / _stopped` |
| handleQuoteUsesEmbeddedContractInfo | ParserAdapter.cpp:256-258 | `quote->getContractInfo()` 非空分支（不查 bd_mgr） |
| orderQueueRejectAndPassThrough | ParserAdapter.cpp:267-287 | actiondate==0、tradingdate==0、合约缺失、成功、`_stopped` |
| orderDetailRejectAndPassThrough | ParserAdapter.cpp:289-309 | 同上四段 |
| transactionRejectAndPassThrough | ParserAdapter.cpp:311-331 | 同上四段 |
| handleParserLogAndSymbolList | ParserAdapter.cpp:333-338 | `handleParserLog`（L335 log_dyn_raw）+ `_stopped` 早退 |
| adapterMgrRegisterAndLookup | ParserAdapter.cpp:341-381 | `ParserAdapterMgr::addAdapter`（NULL/空名/重名/成功）、`getAdapter`（命中/未命中）、`run`、`release` |

## 3. `r11_event_notifier_test.cpp` → `src/WtUftCore/EventNotifier.cpp`

| 用例 | 生产文件:行段 | 打到的分支 |
|---|---|---|
| initDisabledByInactiveFlag | EventNotifier.cpp:36-41, 43-53, 55-58 | 构造/析构 + `init()` 的 `getBoolean("active")` 假分支 |
| 〃 | EventNotifier.cpp:110-113, 138-141, 165-168, 192-196, 211-215 | `notify_log/notify_event/notify(3)/notify(trade)/notify(order)` 的 `_mq_sid==0` 早退 |
| initFailsWhenMqModuleMissing | EventNotifier.cpp:55-81 | `active=true` → 拼路径 → `load_library` NULL → false（用例先自证模块确实不可装载） |
| notifyGuardsBeforeTouchingPayload | EventNotifier.cpp:192-196, 211-215 | payload 非空仍被 guard 拦下（`retainCount()` 不变，证明确实没进 post lambda） |

## 4. `r11_ticker_stramgr_test.cpp` → `WtUftTicker.cpp` / `UftStrategyMgr.cpp` / `WtUftEngine.cpp`

| 用例 | 生产文件:行段 | 打到的分支 |
|---|---|---|
| tickWithoutEngineAndThreadIsSilent | WtUftTicker.cpp:29-40, 46-49, 67-75, 252-257 | 构造/析构、`on_tick` 的 `_thrd==NULL` + `if(_engine)` 假分支、`stop()` 的 `_thrd` 假分支（两次） |
| tickBeforeThreadForwardsButKeepsEngineTime | WtUftTicker.cpp:55-62, 67-75 | `init()`（`get_session_info` + L60 `TimeUtils::getDateTime`）、`on_tick` 早退转发 |
| 〃 | WtUftEngine.cpp:65-88（`set_date_time`/`set_trading_date`/getter 组） | 时间槽不被早退分支推进的可证伪期望 |
| engineTimeSlotsFollowExplicitArguments | WtUftEngine.cpp:65-88 | `rawTime==0 → rawTime=curTime`；显式 rawTime；`set_trading_date` |
| loadFactoriesMissingDirectory | UftStrategyMgr.cpp:28-33 | `StdFile::exists` 假 → false |
| 〃 | UftStrategyMgr.cpp:36-88 | 空目录循环不执行 → true |
| loadFactoriesSkipsNonModules | UftStrategyMgr.cpp:36-60 | 子目录 `continue`、非 `.dylib` 后缀 `continue`、伪 `.dylib` 装载失败 `continue` |
| 〃 | UftStrategyMgr.cpp:90-105 / 107-125 / 127-133 | `createStrategy(fact,unit,id)` 工厂缺失、`createStrategy(name,id)` 的 `ay.size()<2` 与工厂缺失、`getStrategy` 未命中 |
| getStrategyMissReturnsEmptyPointer | UftStrategyMgr.cpp:90-133 | 三处回退 + 空注册表下二次装载 |

## 5. `r11_uftctx_param_log_test.cpp` → `src/WtUftCore/UftStraContext.cpp`

| 用例 | 生产文件:行段 | 打到的分支 |
|---|---|---|
| readParamReturnsDefaultWhenShareOffline | UftStraContext.cpp:734-765 | `read_param` 六个重载（string/int32/uint32/int64/uint64/double）转发 ShareManager |
| syncParamYieldsNullPointerWhenShareOffline | UftStraContext.cpp:767-800 | `sync_param` 六个重载返回 nullptr（含 `bForceWrite=true`） |
| stringWatchParamAndCommitAreNullSafe | UftStraContext.cpp:696-699, 729-732 | `watch_param(const char*, const char*)` + `commit_param_watcher`（L731） |
| DISABLED_R11UftCtxParam.numericWatchParamDereferencesNull | UftStraContext.cpp:700-727 | 五个数值 `watch_param` 重载 `*nullptr` → 段错误（BUG-R11-002），整条禁用 |
| logSinksAndNullStrategyCallbacksAreSilent | UftStraContext.cpp:1014-1030 | `stra_log_info`(L1018) / `stra_log_debug`(L1023) / `stra_log_error`(L1028) |
| 〃 | UftStraContext.cpp:671-692 | `on_params_updated` / `on_session_begin` / `on_session_end` / `on_channel_lost` / `on_position` 的 `_strategy==NULL` 与空实现 |
| idAndTimeForwardingFromEngine | UftStraContext.cpp:802-830 | `id()`、`stra_get_date/stra_get_time(raw)/stra_get_secs` 的时间转发 |

## 6. `r11_trader_reply_deep_test.cpp` → `src/WtUftCore/TraderAdapter.cpp`

| 用例 | 生产文件:行段 | 打到的分支 |
|---|---|---|
| positionResponseFiltersAndGuardsState | TraderAdapter.cpp:1218-1268 | `onRspPosition(nullptr)`、空数组、未知合约 `continue`、多头四元组、`printPosition`(L1401)、sink 双向通知、L1263 状态守卫假分支 |
| positionResponseAfterLoginChainsQueryOrders | TraderAdapter.cpp:1218-1268 | 登录后 `_state==AS_LOGINED` → `AS_POSITION_QRYED` + `queryOrders` |
| orderResponseFiltersBadEntries | TraderAdapter.cpp:1271-1398 | NULL 数组、数组内 NULL 条目、合约缺失 `continue`、死单 `!isAlive()` `continue`、外部 usertag `continue`、活单 `_orders->add` + `_undone_qty`（L1375 SpinLock）、未完成量日志、L1392 状态守卫假分支 |
| orderResponseCancelTallyBranches | TraderAdapter.cpp:1330-1362 | `b_cancels` / `b_auto_cancels`（非 WOF_NOR） / `b_wrongs`（`setError`）三条统计旁支 |
| tradeResponseSkipsItemsWithoutContract | TraderAdapter.cpp:1408-1450 | `getContractInfo()==NULL` 的 `continue`、`s_closetvol` 累计、`_stat_map` 日志循环、L1452 状态守卫假分支 |
| accountResponseHoldsChannelReadyBack | TraderAdapter.cpp:1206-1215, 1263-1267, 1392-1396, 1452-1456 | `onRspAccount` 状态守卫 + 满链就绪（`queryTrades`→`queryAccount`→`on_channel_ready`） |
| pushOrderCancelWrongAndAutoCancel | TraderAdapter.cpp:1492-1735 | 活单建 `_orders`；撤销统计三分支（L1516-1563）；`updateUndone`(L1578)；首推认领块 L1587-1666；`_orders->remove` L1700-1706（L1707 SpinLock）；offset 映射与 sink `on_order` L1720-1735 |
| pushOrderForeignAndEmptyOrderID | TraderAdapter.cpp:1492-1499, 1585-1600, 1690-1700 | 合约缺失早退、外部单不认领、`strlen(orderID)==0` 跳过认领块、平仓首推扣可平今/昨 |
| queriesOnEmptyChannel | TraderAdapter.cpp:232-302, 429-436, 1847-1849, 1154-1157 | `getOrders` 的 `_orders==NULL`、`enumPosition` 空/命中、`getInfos` 未登记、`getBaseDataMgr`、`onRspEntrust(nullptr,nullptr)` |
| logErrorAndLogoutAreNarrow | TraderAdapter.cpp:1852-1855, 1841-1845, 1149-1152 | `handleTraderLog`（L1854 log_dyn_raw）、`onTraderError` 的 `if(err)` 假分支、空 `onLogout` |
| failedLoginKeepsChannelUnready | TraderAdapter.cpp:1133-1147 | `onLoginResult` 失败分支不查持仓；成功后 `AS_LOGINED` + `queryPositions` |
| adapterMgrAddGetRunRelease | TraderAdapter.cpp:1991-2036 | `TraderAdapterMgr::addAdapter`（NULL/空名/重名/成功）、`getAdapter`、`getAdapters`、`run`(L2019)、`release`(L2029) |
