# T5 lane 缺陷清单（TraderYD / TraderXTP / ParserCTP / ParserShm）

只记录，不改产品代码、不改断言凑绿。用例落点见 `../manifests/T5_yd_xtp_ctp_shm.md`。

---

- ID: T5-01
- 状态: 待审核
- 位置: `src/TraderYD/TraderYD.cpp:226-239`（构造函数初始化列表）
- 现象: `TraderYD()` 只初始化了 11 个成员，`m_sink`（裸指针）、`m_lDate`、`m_orderRef`、`m_bUpdateCache` 都是**不确定值**。
  类里 30 多处 `if (m_sink)`、`write_log(m_sink, ...)` 直接读 `m_sink`；`makeEntrustID()` 用未初始化的 `m_orderRef`
  做 `fetch_add`；`makeOrderInfo/makeTradeRecord` 用未初始化的 `m_lDate` 拼订单/成交时间戳；
  `notifyOrder/notifyTrade` 用未初始化的 `m_bUpdateCache` 决定要不要走本地缓存链（走进去就会 `m_mapPosition->get(...)` 空指针）。
- 影响: 未 `registerSpi()` 就收到厂商回调 = 野指针解引用；未登录就推单 = 委托号/时间戳不确定；`m_bUpdateCache` 的随机值会让同一份代码在不同进程里走不同分支。
- 用例侧回避: T5 的 YD 夹具在 `SetUp()` 里强制 `init(cfg)`（覆盖 `m_bUpdateCache`）+ `registerSpi(&sink)` 后才驱动回调；
  并**不对** `orderDate/orderTime/委托号自增起点` 下断言。
- 建议: 初始化列表补 `m_sink(NULL), m_lDate(0), m_orderRef(0), m_bUpdateCache(false)`。

---

- ID: T5-02
- 状态: 待审核
- 位置: `src/TraderXTP/TraderXTP.cpp:901-951`（`queryAccount/queryPositions/queryOrders/queryTrades`）
- 现象: 这四个入口直接 `_api->QueryAsset(...)` / `_api->QueryPosition(...)`，**没有** `_api == NULL` 守卫，
  而同文件的 `login/logout/orderInsert/orderAction`（824/835/844/881）都有守卫；`isConnected()` 也只读状态位。
- 影响: 未 `connect()`（或厂商 dylib 缺失导致 `_api` 一直为 NULL）时调用查询 → 空指针解引用。WtTraderBroker/TraderAdapter 的
  定时查询链正是无条件调这四个方法的，等于「先查询后连接」的用法必崩。
- 用例侧回避: `t5_xtp_api_test.cpp` 的 `DISABLED_query_apis_dereference_null_api_without_guard`，整条不启用。

---

- ID: T5-03
- 状态: 待审核
- 位置: `src/TraderXTP/TraderXTP.cpp:363-374`（`OnOrderEvent` 错误分支）
- 现象: `makeEntrust(order_info)` 在合约未登记时返回 NULL（`TraderXTP.cpp:199-200`），但调用方既不判空也不换分支：
  `_sink->onRspEntrust(entrust, error)` 先传 NULL，紧接着 `entrust->release()` → 空指针调用；
  且 `_sink` 也未判空（同函数 380 行的正常分支却判了）。
- 影响: 柜台推一条「被拒 + 本地合约表里没有该代码」的回报（换日/新开品种最常见）即崩。
- 用例侧回避: `DISABLED_order_event_error_branch_releases_null_entrust`；启用版 `order_event_with_error_converts_entrust_and_error`
  用已登记合约只覆盖安全面。

---

- ID: T5-04
- 状态: 待审核
- 位置: `src/Share/WtKVCache.hpp:191-197`（`get`）与 `:198-230`（`put`/`put_if_none`）
- 现象: `get()` 靠 `_indice` 未命中返回 `""`，**不碰** `_cache._block`，所以在未 `init()` 的缓存上是安全的；
  `put()` 则直接 `_cache._block->_size` / `->_capacity` → 未 init 即空指针解引用。
- 影响: 两个交易适配器都在「登录成功之前」就可能被调用写缓存：
  `TraderYD.cpp:811`（`orderInsert` 带 userTag 时 `m_eidCache.put`）、`TraderXTP.cpp:866`（同上）、
  `TraderXTP.cpp:290`（`makeOrderInfo` 里 `m_oidCache.put`）。`m_eidCache/m_oidCache` 只在 `notifyLogin/doLogin` 成功路径里 init。
- 用例侧回避: 本轮用例不触发任何 `put` 路径（`orderInsert` 都在守卫处被拒），`get` 路径被大量覆盖并验证「未命中→空串→userTag 回落 entrustID」。

---

- ID: T5-05
- 状态: 待审核
- 位置: `src/TraderXTP/TraderXTP.cpp:563-566`（`OnQueryAsset`）
- 现象:
  `if (asset->deposit_withdraw > 0) setDeposit(x); else if (asset->deposit_withdraw < 0) setWithdraw(0);`
  出金分支把金额写死成 0，丢掉了 `-deposit_withdraw`。同时 `setFrozenCommission(0)`/`setCloseProfit(0)`/`setDynProfit(0)` 也是硬置。
- 影响: 资金查询回报里的出金永远是 0，依赖 `on_account(deposit/withdraw)` 的策略/对账口径失真。
- 用例侧回避: `query_asset_negative_deposit_withdraw_currently_lost` 是**现状断言**（`EXPECT_DOUBLE_EQ(0.0, withdraw)`）；
  修复后这条用例应改为期望 `8000.0`，届时它会失败——这是设计好的哨兵，不要删。

---

- ID: T5-06
- 状态: 待审核
- 位置: `src/TraderXTP/TraderXTP.cpp:360`、`:369`、`:668`、`:762`、`:802` 等（`write_log(_sink, LL_ERROR, error_info->error_msg)`）
- 现象: 厂商返回的自由文本被当作 fmt 的**格式模板**传入（`write_log` 内部 `fmtutil::format(format, args...)`）。
  `fmt::format` 对运行期格式串里的 `{`/`}` 会抛 `fmt::format_error`（或 `on_error` 终止），而这条路径上没有 try/catch
  （`TraderYD.cpp:34-43` 的 write_log 同形，但它至少都传了字面量模板）。
- 影响: 柜台文案里出现花括号（例如 `{"code":1}` 之类 JSON 错误体）→ 日志调用抛异常 → 回调线程终止。
- 用例侧回避: `on_error_routes_vendor_message_into_trader_log` 只用**不含花括号**的文案；带花括号的注入用例未启用（会直接抛异常，属挂死/崩溃面）。

---

- ID: T5-07
- 状态: 待审核
- 位置: `src/TraderXTP/TraderXTP.cpp:118-130`（`wrapOffsetType(XTP_SIDE_TYPE, XTP_POSITION_EFFECT_TYPE)`）
- 现象: 第二个 `else if (XTP_POSITION_EFFECT_CLOSE == offType) return WOT_CLOSE;` 已经吃掉 CLOSE，
  其后 `else if (XTP_POSITION_EFFECT_CLOSE == offType) return WOT_CLOSEYESTERDAY;`（124 行）条件与上面完全相同 → **永不可达**（死代码）。
- 影响: 回读路径上平昨不可能被还原成 `WOT_CLOSEYESTERDAY`；与 `:104-116`（WOT_CLOSEYESTERDAY→XTP_POSITION_EFFECT_CLOSE）
  合起来形成「平昨→平今语义」的有损往返，期权/期货场景下持仓开平口径会与下单侧不一致。
- 用例侧回避: `t5_xtp_map_test.cpp` 的 `offset_type_two_way_and_closeyesterday_alias` 断言 `WOT_CLOSE`（现状）并注释了不可达分支。

---

- ID: T5-08
- 状态: 待审核
- 位置: `src/ParserCTP/ParserCTP.cpp:426-429`（`IsErrorRspInfo`）
- 现象: 形参被 `(CThostFtdcRspInfoField */*pRspInfo*/)` 完全忽略，函数体 `return false;`。
- 影响: `OnRspError`（170-173）变成一个**纯空函数**（CTP 的错误回报被静默丢弃，日志里查不到任何错误码）；
  `OnRspUserLogin`（186-206）的 `!IsErrorRspInfo(...)` 永真 → 登录失败也会按登录成功推进（继续 `DoSubscribeMD`）。
  同名函数在 ParserCTPMini/ParserCTPOpt 里是**有实现**的（比对 `ErrorID != 0`），说明这里是被漏掉的退化。
- 用例侧回避: `rsp_error_publishes_nothing_at_all` 断言「错误回报既不落日志也不发事件」= 现状；
  `rsp_user_login_ignored_until_last_packet` 只覆盖 `bIsLast=false`。

---

- ID: T5-09
- 状态: 待审核
- 位置: `src/ParserCTP/ParserCTP.cpp:186-206`（`OnRspUserLogin`）
- 现象: 成功分支里 `m_pUserAPI->GetTradingDay()`（190 行）无判空，而同文件 `ReqUserLogin`（368-373）、`DoSubscribeMD`（408）都判了。
- 影响: `init()` 失败/被跳过（`m_pUserAPI==NULL`）时若厂商仍回调登录响应 → 空指针解引用。
- 用例侧回避: `DISABLED_rsp_user_login_last_packet_dereferences_null_api`。

---

- ID: T5-10
- 状态: 待审核
- 位置: `src/ParserCTP/ParserCTP.cpp:59-77`（`strToTime`）
- 现象: 数字字符被无界写入文件级 `static char tmp_timestr[10]`，`idx` 没有上界检查（>9 个数字即越界写 `.bss`）；
  同时该缓冲是**全局静态**，函数不可重入、多线程（多合约并行解析）互相踩。
  另外 `constexpr inline` 修饰一个写全局状态的函数，语义上是矛盾的（编译期求值会污染静态区）。
- 影响: 柜台时间串带毫秒（`"09:30:15.450"`=10 位数字）就越界；`UpdateTime` 是厂商字段，长度不受本侧约束。
- 用例侧回避: `depth_maps_core_fields_into_tick_struct` 用标准 8 字符 `"09:30:15"`；带 10 位数字的越界用例未启用。

---

- ID: T5-11
- 状态: 待审核
- 位置: `src/ParserCTP/ParserCTP.cpp:114-141`（`init`）
- 现象: macOS 分支 `m_funcCreator = CThostFtdcMdApi::CreateFtdcMdApi;` 之后立刻用真实厂商 API 建实例，
  并在 `m_strFlowDir/m_strBroker/m_strUserID` 拼出的相对路径上 `fs::create_directories`（114-118）——
  即「init() 必然在进程 cwd 造目录 + 造厂商实例」，`connect()` 又会让它起 IO 线程，`disconnect()→Release()` 会销毁它。
- 影响: 本轮门禁要求「不起线程、不连真柜台」，所以 `init/connect/disconnect/release` 的**真实生命周期**（136-138、152、161-164）无法在 T5 覆盖；
  非 macOS 分支的 `dllpath/get_symbol` 逻辑（122-134）在 macOS 上被 `#ifdef` 排除，属结构性不可达。
- 处置: 留白，**请主控裁定**（是否允许在临时 cwd 建目录的一次性 init 用例）。

---

- ID: T5-12
- 状态: 待审核
- 位置: `src/ParserShm/ParserShm.cpp:63-69`（构造函数）
- 现象: 初始化列表缺 `_bd_mgr`、`_gpsize`、`_cpu`。`_bd_mgr` 只在 `registerSpi` 里赋值（326），
  `_gpsize` 只在 `init` 里赋值（79），而 `init` 之前 `connect()` 就能被调用。
- 影响: ①未 `registerSpi` 就 `subscribe/unsubscribe` → 用野指针调 `getContract`/`getGlobalSize`（293、307、287）；
  ②`_gpsize` 为不确定值时接收循环里 `recv_cnt % _gpsize`（188、212、235、258）命中 0 → **整数除零 → SIGFPE**，
  而且这是**每条回报都会走**的取模，随机崩、难复现。
- 用例侧回避: YD/SHM 夹具一律先 `init()+registerSpi()`；`DISABLED_subscribe_without_register_spi_uses_uninit_bd_mgr` 记录①。

---

- ID: T5-13
- 状态: 待审核
- 位置: `src/ParserShm/ParserShm.cpp:292-293`（`subscribe`）与 `:306-307`（`unsubscribe`）
- 现象: `auto ay = StrUtil::split(code, ".")` 之后直接取 `ay[1]`/`ay[0]`，没有校验 `ay.size() >= 2`；
  `code` 也不判空。传入不含 `.` 的原始代码（`"ag2512"`）→ `ay` 只有 1 个元素 → `ay[1]` 越界读。
- 影响: 上层（ParserAdapterMgr/QuoteFactory）传进来的代码串不保证是标准码，一条脏配置就能读到越界字符串。
- 用例侧回避: 启用用例只喂 `"交易所.代码"` 形状；`subscribe_splits_std_code_into_raw_code_and_exchange`
  与 `subscribe_uses_second_segment_only` 锁定「取第 2 段」的现状语义（`"SHFE.ag.2512"` → 查 `ag@SHFE`）。

---

- ID: T5-14
- 状态: 待审核
- 位置: `src/ParserShm/ParserShm.cpp:297`（配合 `:327`）
- 现象: `registerSpi` 用 `getGlobalSize()` 给 `_subbed` 定尺寸；若数据底座返回 0（或根本没登记任何合约），
  `_subbed` 是空 `vector<bool>`，此时 `subscribe` 里 `if (_subbed.empty()) resize(getGlobalSize())` 仍然 resize(0)，
  随后 `_subbed[cInfo->getTotalIndex()] = true` 在空 vector 上下标写 → 越界写（`operator[]` 无检查）。
  `unsubscribe`（311）同形。
- 影响: 合约表加载顺序如果晚于 parser 订阅，就是内存踩踏而不是干净报错。
- 用例侧回避: `DISABLED_subscribe_with_zero_global_size_writes_out_of_range`（含可复现的 `ZeroSizeShmSink`，整条不启用）。

---

## 本轮「未进入」的面（不是缺陷，是环境/形状限制，交主控裁定）

1. **ParserShm 接收状态机全部**（`ParserShm.cpp:92-269`）：`connect()` 起线程 + `BoostMappingFile::map()` 真实映射文件 +
   `while(!StdFile::exists)` 每秒轮询 + `_readable/_writable/pid` 追赶 + `lastIdx % _capacity` 取模落位 +
   `_type` 四类分发 + `CpuHelper::bind_core`。按 lane 约定不建真实共享内存段、不起线程 → 留白
   （`DISABLED_receive_path_needs_a_real_mapped_file`）。本轮只覆盖 `ParserShm.h:30-59` 的队列布局（纯堆内存即可构造）。
   参考提交 `c3525f22`（drop environment-specific shm test）。
2. **TraderYD 的真实链路**：`connect()`（681-711，调用 `m_funcCreator`）与 `doLogin()`（756-770，`m_pUserAPI->login`）
   会调用 NULL/厂商函数指针；`notifyLogin(0,...)`（260-293）与 `notifyFinishInit`（346-423）都解引用 `m_pUserAPI`。
3. **TraderYD::notifyOrder 的缓存更新链**（`TraderYD.cpp:432-516`）：`m_bUpdateCache=true` 或 `m_bCatchup=false` 才进，
   进去后 447/484 行 `m_mapPosition->get(key)` 与 448/485 行 `pos->getPrePosition()` **既不判 map 空也不判 pos 空** →
   未跑 `notifyFinishInit`（本机跑不了）就推一条平仓委托即空指针。故本轮只覆盖 `m_bCatchup=true && m_bUpdateCache=false`
   的「转换 + 主推」分支。该 pos 判空缺失同时按缺陷对待，见 **T5-15**。
4. **TraderXTP::connect/reconnect/doLogin**（581-686、753-817）：NULL creator、`fs::create_directories`、真实 Login。

---

- ID: T5-15
- 状态: 待审核
- 位置: `src/TraderYD/TraderYD.cpp:444-474`、`:481-513`
- 现象: 订单回报里冻结/释放可用手数的两个分支，直接
  `WTSPositionItem* pos = (WTSPositionItem*)m_mapPosition->get(key);` 后使用 `pos->getPrePosition()`，
  既不判 `m_mapPosition`（只在 `notifyFinishInit` 里创建，376-377），也不判 `pos` 是否命中。
  对照同文件成交回报分支（551-558）是**判了** `pos == NULL` 并现场创建的 —— 说明订单分支是漏写。
- 影响: 追数据阶段（`m_bCatchup=false`）若 `notifyFinishInit` 没有先建好该 key 的持仓项（例如当日新开的仓被平），
  或 `update_cache=true` 时上表任何一条平仓委托 → 空指针解引用。
- 用例侧回避: 见上面「未进入的面」第 3 条，该分支整条未启用。
