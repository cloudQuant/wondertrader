# T2 候选复核 — TraderCTP / ParserXeleSkt / ParserCTPOpt

本文件列出的 T2-CTP-01~09、T2-XELE-01~04、T2-OPT-01~06 最初按源码、接口和测试源码静态复核；该阶段没有运行测试或复现崩溃。2026-10-29 后续对 T2-OPT-03 和 T2-XELE-01 运行了本地定向测试，结果见各条；其他条目仍按静态证据记载，不代表本轮已运行。厂商 SDK 头文件只给出回调签名，没有给出本文件关心的指针可空契约；对这一点保留待验证结论。

---

## TraderCTP（src/TraderCTP/TraderCTP.cpp）

- **T2-CTP-01 — 部分确认：登录前生成 ID 会读取未初始化的会话字段**
  - 源码锚点：构造函数 TraderCTP.cpp:203-217 未初始化 m_frontID、m_sessionID、m_orderRef；makeEntrustID 在 :381-391 无状态检查地读取并递增这些字段。m_lDate 也未初始化，但不是 makeEntrustID 读取的字段。
  - 调用前提：ITraderApi::makeEntrustID（src/Includes/ITraderApi.h:229-232）没有登录前置条件。适配器 TraderAdapter::doEntrust（src/WtCore/TraderAdapter.cpp:487-508）先调用 makeEntrustID，再进入 orderInsert 的就绪状态检查（TraderCTP.cpp:471-475）；适配器的 openLong/openShort/closeLong/closeShort 会直接到 doEntrust（TraderAdapter.cpp:1331-1398）。因此，调用方在登录前提交交易请求时，前置 ID 生成已经读取未初始化值，之后才会被下单检查拒绝。
  - 测试边界：make_entrust_id_is_fixed_width_and_increments_ref 先执行 bringToReady()（t2_traderctp_conversion_test.cpp:597-613），没有覆盖登录前调用。
  - 判定：这是有条件可达的未初始化读取风险；目前未在运行环境复现。建议给所有会话字段明确初始化，并在尚无登录会话时拒绝生成订单 ID；增加登录前调用测试，验证失败返回且不会读取未初始化状态。

- **T2-CTP-02 — 确认失败处理缺口；m_sink 部分受适配器顺序保护**
  - 源码锚点：非 Apple init() 只查找 creator 符号、不检查句柄/函数指针，并始终返回 true（:257-278）。connect() 在 :310-311 直接调用 creator 并解引用结果；注册 front 时 :326 无条件解引用 m_sink。:329-332 的 API 判空晚于前面的调用。
  - 接口路径：标准 TraderAdapter::run() 在 src/WtCore/TraderAdapter.cpp:404-416 先 registerSpi(this) 再 connect()，因此此路径通常有 sink；TraderCTP 构造函数本身没有初始化 m_sink，直接使用插件 API 或注册顺序错误时可能解引用未初始化/无效指针。缺库/缺符号或 creator 返回空 API 的失败路径未被检查。
  - 测试边界：T2 的 DISABLED_connect_starts_worker_thread_and_needs_real_front 没有调用真实连接，本轮也未运行它；没有证据证明本机一定缺库或已发生崩溃。
  - 判定：creator/API 缺失时的空指针路径是源码可确认的失败处理缺陷；“m_sink 在标准适配器必为空”则是误报。建议 init/connect 显式检查库、符号和 creator 返回值，失败时记录错误并停止后续初始化；日志调用做 sink 守卫，并通过可注入 creator 的测试覆盖失败分支。

- **T2-CTP-03 — 原“未连接时资源泄漏”表述不成立；对象复用的队列/停止状态缺陷成立**
  - 源码锚点：disconnect() 无条件将 release() lambda 压入 m_queQuery，只有存在 worker 才 join（:368-379）。未 connect 时 API 尚未创建（创建发生在 connect():310），所以没有已创建 API 被泄漏；队列项会留存。若随后 connect，worker 可以执行这条旧 release，释放新建 API。正常 connect→disconnect 后 release() 将 m_bStopped=true（:281-290），而 connect() 不重置该标记、worker 的循环条件是 while (!m_bStopped)（:334-364），同一对象再次 connect 时 worker 会立即退出。另外 worker 对队列的 empty/front 读取发生在锁外（:339-355），disconnect() 的 push 也没有持有 m_mtxQuery（:370-372）；并发断开/队列处理存在数据竞争。
  - 测试边界：T2 连接用例是 DISABLED；现有测试没有验证断开前未连接后再连接或同对象重连。
  - 判定：原条目声称未连接直接 disconnect 会泄漏 API 是误报；同一条目应保留为生命周期/重连缺陷，条件是对象被复用，并避免 worker 与 disconnect 并发改队列。建议让 disconnect 在无 worker 时不投递延迟释放；定义单次使用或支持重连的状态机，统一队列锁定，清理过期队列，并在重新启动 worker 前正确重置停止状态。用可替代 API 的测试覆盖两种时序。

- **T2-CTP-04 — 条件性日期校验风险，柜台输入边界未证实**
  - 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
  - 源码锚点：登录回调 :742-754 对 GetTradingDay() 使用 atoi；空字符串会得到 0，空指针则会触发未定义行为。查询确认回调 :814-827 在 ConfirmDate 转换值 >= m_lDate 时进入 WS_ALLREADY 并报告登录成功。
  - 证据边界：如果登录成功响应的 TradingDay 为空，比较确实可能把非法/空确认日期（转换为 0）当作已确认；但当前 SDK 头文件和本地测试没有证明成功响应会给出空 TradingDay，也没有厂商契约说明该字段可空。T2 测试 bringToReady() 使用 20240115，未覆盖空日期。
  - 判定：不能据此声称常规登录必然误判；属于输入校验缺口，运行时可达性待验证。建议校验非空、8 位且有效的交易日和确认日，校验失败时保持未就绪并记录错误；增加空、非法和较旧日期用例。

- **T2-CTP-05 — 未检查回调载荷；厂商是否允许 NULL 尚待接口契约确认**
  - 源码锚点：OnRspOrderInsert、OnErrRtnOrderInsert 分别在 :861-879、:1397-1408 把字段交给 makeEntrust；makeEntrust 立即读取 InstrumentID/ExchangeID（:1263-1267）。OnRspOrderAction 错误分支调用 makeAction，后者直接读取字段（:882-897, :1303-1311）。OnRtnOrder/OnRtnTrade 经 makeOrderInfo/makeTradeInfo 读取回调参数（:1164-1188 及转换函数）。
  - 证据边界：CTP 6.3.15/6.6.9 本地头文件仅列回调签名，没有声明这些参数是否可空。T2 的 rsp_order_insert_and_action_error_paths 只传非空结构（:875-928）；这既不能证明 NULL 合法，也不能证明不可能。
  - 判定：静态上存在无保护解引用；“CTP 在部分拒绝响应里允许 NULL”尚无本地契约或实测支持。标为待接口契约验证的防御缺口，不以当前证据确认供应商回调一定会崩。建议确认各回调的空载荷语义；若允许空载荷，先判空并独立上报 pRspInfo 错误，若不允许，也应在边界记录异常并避免将 NULL 传入转换函数。补齐对应的 NULL/错误组合测试。

- **T2-CTP-06 — 查询末帧无条件解引用；NULL 末帧是否符合厂商协议待证**
  - 状态: 已合并（迭代02：主编号 BUG-T25-003）
  - 源码锚点：OnRspQryTradingAccount 在 bIsLast && !IsErrorRspInfo 时直接读取 pTradingAccount（:899-930）。OnRspQrySettlementInfo 虽在累加内容时判了 pSettlementInfo，但只要已有内容且末帧到达，就在 :1093-1095 再读 pSettlementInfo->TradingDay，没有保护；对比持仓回调 :932-940 对行字段有判空。
  - 测试边界：T2 的 rsp_account_position_and_settlement 使用非空账户及结算行（:930-1000）。T25 文件中另有禁用用例 DISABLED_settlement_info_last_row_without_payload_crashes（:1954 附近）；它是测试源码而非本轮运行结果。SDK 签名未描述无结果末帧如何传参。
  - 判定：是源码可确认的条件性空指针风险，但“空查询必定以 NULL 收尾”证据不足。建议账户末帧无行时返回受控空结果/错误；结算查询在收到有效行时缓存日期，末帧只使用缓存值，NULL 末帧不要解引用；按厂商契约补空查询、NULL 末帧测试。

- **T2-CTP-07 — 合约状态强转是误报；订单状态 b/c 存在语义错映射**
  - 源码锚点：合约状态在 :1411-1415 直接强转。CTP 本地类型头将 THOST_FTDC_IS_BeforeTrading...Closed 定义为字符 '0'..'6'（src/API/CTP6.3.15/ThostFtdcUserApiDataType.h:985-1000），WTSTradeStatus 对应值也为 '0'..'6'（src/Includes/WTSTypes.h:360-378），所以此处对本地 CTP 状态集是对齐的；原文所称 '1'..'A' 错误。
  - 订单状态在 wrapOrderState（:167-178）将多数值直接强转。CTP 定义 Unknown='a' / NotTouched='b' / Touched='c'（ThostFtdcUserApiDataType.h:552-558），WTS 定义 Submitting='a' / Cancelling='b' / Nottouched='c'（WTSTypes.h:250-261）。Unknown 被显式映射到 Submitting；但 b 被解释为 Cancelling，c 被解释为 Nottouched，与 CTP 语义不符，而且 WTS 没有 Touched 状态。
  - 测试边界：T2 覆盖 Unknown 和常见订单状态（t2_traderctp_conversion_test.cpp:756-820），不覆盖 NotTouched/Touched；instrument status 测试 :1074-1083 只检查 Closed 的数值相等。
  - 判定：保留为订单状态窄范围映射缺陷；合约状态强转部分应撤销。建议显式逐值映射并定义 WTS 如何表示 Touched；新增 b/c 映射测试，避免未来 CTP 新状态落入不明 default。

- **T2-CTP-08 — “与 makeEntrustID 序列不同步”不准确；无 ID 的裸下单分支可能复用 OrderRef**
  - 源码锚点：orderInsert() 在空 userTag 分支用 m_orderRef.fetch_add(0)（:485-496），本身不递增。标准 TraderAdapter::doEntrust() 先调用 makeEntrustID() 再写入非空 userTag（src/WtCore/TraderAdapter.cpp:487-508），因此标准适配器订单走解析 entrustID 的分支，序列并非不同步。
  - 条件风险：直接调用 ITraderApi::orderInsert() 且既未生成新 entrustID、也未设置 userTag 时，每次只读取同一个当前引用，可能重复；接口注释没有写明 orderInsert 必须先 makeEntrustID。T2 的 order_insert_cffex_anyprice_and_close_offsets 确实多次裸调用，但没有断言各次 OrderRef（:666-699）。
  - 判定：标准适配器重复号主张是误报；裸 API 用法的唯一性依赖未写明的调用前提，按条件性缺陷/契约不清记录。建议确定并文档化前置 ID 契约；对缺少有效 ID 的委托要么拒绝，要么在下单入口原子分配新号，同时避免对已由 makeEntrustID() 分配的号二次递增。补逐次 OrderRef 唯一性测试。

- **T2-CTP-09 — 测试缺口条目已过时，剩余是非产品问题**
  - 状态: 已撤回（原覆盖缺口陈述已过时；unused helper 的测试维护不构成产品缺陷）
  - 源码/用例锚点：makeAction 没有所谓“正常分支”；它由 OnRspOrderAction 错误回调使用，T2 的 rsp_order_insert_and_action_error_paths 已以非空结构间接覆盖（:911-927）。wrapTimeCondition 在生产文件标为 [[maybe_unused]] 且没有调用点，是未使用 helper，不是交易行为缺陷。插件 C 入口测试也已出现在 T25 的 plugin_entry_points_build_and_destroy_trader（t25_traderctp_gap_test.cpp:595-608）。
  - 判定：原文“本轮未建 C 入口测试”和“makeAction 正常分支未覆盖”均过时/描述错误；private 不是产品 bug。测试源码存在不等于本轮测试已执行。建议从缺陷清单移出；若覆盖率目标仍要求直接验证 unused helper，应先确认是否删除或重新接入，而不是添加无业务意义的测试 seam。

---

## ParserXeleSkt（src/ParserXeleSkt/ParserXeleSkt.cpp）

- **T2-XELE-01 — 配置错误经 bool 接口抛异常；需确认外层调用者异常边界**
  - 源码锚点：init() 在 :99-116 对配置地址直接调用 ip::make_address(_tcp_host)，无异常转换。IParserApi::init 文档契约是 bool 成功值（src/Includes/IParserApi.h:88-95）。WtDtServo 的 ParserAdapter::init 在 src/WtDtServo/ParserAdapter.cpp:149-154 直接调用 _parser_api->init(cfg)，本层没有 catch；WtDtRunner 在 src/WtDtServo/WtDtRunner.cpp:397-399 也没有局部 catch。
  - 测试边界：旧用例把抛异常作为现状断言；现改为缺失或非法 host 应受控返回失败。两条本机定向测试均 FAIL，实际抛 `boost::system::system_error`；有效地址初始化对照 PASS。更上层是否统一捕获仍未查明，没有启动网络。
  - 判定：插件将配置错误作为异常而不是初始化失败返回，属于接口错误路径不一致；不能仅凭本地路径断言整个进程必崩。建议捕获 boost::system::system_error，记录配置错误并返回 false；调整测试为错误配置返回失败、不越过适配器边界抛出。

- **T2-XELE-02 — release/disconnect 没有收束接收线程，存在可静态确认的析构风险**
  - 源码锚点：release() 空实现（:119-122）；connect() 成功后创建 _thrd_parser 执行 io_context::run（:265-277）；disconnect() 关 socket、置 _stopped 并 post 事件，但不 join/reset 线程（:279-292）。handle_udp_read 会读取 _stopped（:341-350），所以原文“从没被循环读取”错误。析构为空（:95-97），而 StdThread 是 std::thread（src/Share/StdUtils.hpp:21-24）；若对象销毁时线程对象仍 joinable，标准线程析构会终止进程。
  - 调用路径/证据边界：WtDtServo 的 ParserAdapter::release() 只调 parser 的 release() 再销毁对象（src/WtDtServo/ParserAdapter.cpp:205-217）；因此正常 teardown 也没有在适配器侧补 disconnect/join。T2 的连接用例 DISABLED，R76 只测未 connect 的路径。本轮没有启动线程或网络，结论为源码生命周期缺陷，未实际观察到 terminate。
  - 判定：真实生命周期缺陷；原文“stopped 从未读取”及“connect 已实测会挂”应撤销。建议 release/disconnect 幂等地取消接收、停止 io_context 并 join；析构复用同一安全清理逻辑；若支持重连，再明确 reset io_context 和状态。用可注入/本地环回接收器验证 stop、join、重复 disconnect 与重连。

- **T2-XELE-03 — _bd_mgr 未初始化/注销时未清空，生命周期条件下可空读或悬空读**
  - 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
  - 源码锚点：构造函数初始化 _sink 但不初始化 _bd_mgr（:84-91；声明在 ParserXeleSkt.h:82-85）；registerSpi() 仅在 listener 非空时赋值 _bd_mgr，传 NULL 只清 sink（:318-329）。prepare() 直接用 _bd_mgr（:202），行情缓存路径也可能调用 _bd_mgr->calcTradingDate（:402-405, :462-465）。
  - 调用前提：标准 ParserAdapter 会在 init 前注册 SPI（src/WtDtServo/ParserAdapter.cpp:149-154），所以正常入口有赋值；但接口没有要求所有调用者必须如此。注销 sink 后继续接收，或先注册再注销且管理器已析构，保留未清空/悬空管理器风险。
  - 测试边界：R76 的 RegisterSpiNullMakesNextRegistrationSilent 明确记录 registerSpi(nullptr)“只清 _sink”（r76_xele_boundary_test.cpp:141-161），但不验证 _bd_mgr 安全；T2/R76 均未进入 packet decode。
  - 判定：条件性生命周期缺陷，不能说“未注册时一定是 NULL”（它实际是未初始化值）。建议构造时置空；注册 NULL 时清空 _bd_mgr；在 prepare/解包前验证依赖存在，并确保停止回调后才释放管理器。更新 R76 测试契约。

- **T2-XELE-04 — 行情报文转换未覆盖：测试缺口，不是已确认产品缺陷**
  - 状态: 已撤回（覆盖缺口不作为产品缺陷；报文转换仍需测试）
  - 源码锚点：handle_udp_read/prepare/reconnect/extract_buffer 为 private（ParserXeleSkt.h:50-61）；快照和深度报文到 WTSTickData 的路径在 ParserXeleSkt.cpp:156-262, 332-358, 360-524，测试当前没有可注入的报文字节入口。
  - 测试边界：T2 文件说明只覆盖 IParserApi 公开面（t2_parserxele_lifecycle_test.cpp:1-10）；R76 也明确跳过需要真实组播和 io_context 的路径（r76_xele_boundary_test.cpp:16-18）。本轮没有用网络或构造 packet 验证这些转换。
  - 判定：覆盖率/可测性缺口，不足以推出行情转换错误。建议抽取纯报文解码函数或加入仅测试可用的 datagram 注入 seam，使用固定快照/深度报文验证交易日回退、价格缩放、字段赋值和截断报文；无需把内部成员整体改为 public。

---

## ParserCTPOpt（src/ParserCTPOpt/ParserCTPOpt.cpp）

- **T2-OPT-01 — loader/creator 未判空；缺库或缺符号时存在确定的空函数调用路径**
  - 源码锚点：init() 在 :110-134 加载模块、取 symbol 后不校验 m_hInstCTP/m_funcCreator，立即调用 creator 并解引用返回 API；函数最后无条件返回 true。
  - 证据边界：缺库/符号时的失败路径由源码可确认，但本轮未检查本机动态库状态、未调用 init 的该路径、未复现崩溃。T2 的 DISABLED_init_and_login_rsp_dereference_missing_ctpopt_library 只是禁用用例和注释（t2_parserctpopt_tick_test.cpp:427-434）。R105 的 init_flowdir_creation_failure_throws_before_vendor_symbol 只验证目录创建异常（r105_parserctpopt_api_test.cpp:319-336），其余测试注入 stub API；并非 loader 失败验证。
  - 判定：缺库/缺符号时的失败处理缺陷成立；“macOS 本机无库、init 必崩”和“本目录所有测试都没调用 init”没有本轮证据，后者也被 R105 的前置目录错误用例修正。建议逐层判空并释放句柄，向 IParserApi 返回 false、记录失败原因；用可替代 DLLHelper/creator 的 stub 覆盖缺库、缺符号、creator 返回 NULL。

- **T2-OPT-02 — 登录回调在 API 已 teardown 的竞态风险；普通未初始化对象不会自然收到该回调**
  - 源码锚点：成功登录回调在 :180-193 解引用 m_pUserAPI->GetTradingDay()；disconnect() 先 RegisterSpi(NULL) 再 Release() 并置空（:152-161）。成功回调来自该 API；是否可能在 unregister/release 之后仍有在途回调，当前本地 SDK 头文件没有说明。
  - 测试边界：T2 的登录测试只覆盖 bIsLast=false（:270-279）；T2 的 DISABLED 用例假设空 API 回调会崩，但没有运行。R105 的 injected_api_connect_login_and_do_subscribe 通过非空 stub 调用完整成功回调（:243-281）。
  - 判定：原文“未 init 成功却收到登录回报”不是标准调用流，不能当作确认产品缺陷；保留 teardown 与在途回调的竞态待验证。单纯加空指针判断不足以消除可能的数据竞争/对象释放竞态。建议依据厂商释放语义建立回调停止/排空或同步生命周期，并通过并发 teardown stub 验证。

- **T2-OPT-03 — 错误回报被吞：生产缺陷；测试 oracle 已拆分，登录错误契约仍失败**
  - 源码锚点：IsErrorRspInfo 无条件返回 false（:402-405）；OnRspError 只调用该 helper 丢弃结果（:164-167）。因此登录错误分支也不会被识别（:180-193）。即使 helper 修好，订阅响应两个分支仍为空（:326-336），退订回调也为空（:213-216）。
  - 测试边界：旧的 `heartbeat_and_error_callbacks_do_not_reach_core` 已拆为 `heartbeat_warning_logs_without_event` 与 `error_callbacks_are_safe_and_do_not_publish_quotes`（`t2_parserctpopt_tick_test.cpp:248-268`）；后者检查异常安全和不发布行情，不再断言错误必须静默。新增 R105 `nonzero_login_error_does_not_report_success_or_subscribe` 用 ErrorID=404 验证登录错误不应报成功或订阅；零码成功由独立对照用例覆盖。
  - 判定：错误码被忽略是明确产品缺陷；订阅/退订错误当前也没有可见处理，不能仅修 helper 后就认为修好。建议对空 pRspInfo 安全判定、正确识别 ErrorID，并在登录/订阅/退订回调中记录或传递失败；反向修改测试断言检查错误被观测到。
  - 后续定向验证（2026-10-29）：重建后四条相关测试为 3 PASS / 1 FAIL；`nonzero_login_error_does_not_report_success_or_subscribe` FAIL，实际仍报告成功登录并下发订阅。零码登录、心跳和错误回调对照 PASS。测试使用本地 API stub，未调用厂商运行时；订阅/退订错误处理的完整契约及厂商行为仍未验证。

- **T2-OPT-04 — 登录后即时订阅使用固定 500 项栈数组，超量无边界检查**
  - 源码锚点：subscribe() 在 m_uTradingDate != 0 分支声明 char* subscribe[500]，对所有 vecSymbols 无界写入（:407-430）。登录前缓冲分支只保存集合（:409-412）；另一个首次下发函数 SubscribeMarketData() 用按集合大小分配的数组（:365-400）。
  - 测试边界：R105 覆盖登录后内联下发，但只使用两个代码（r105_parserctpopt_api_test.cpp:283-317）；没有 >500 用例。IParserApi 的 CodeSet 没有在接口处声明 500 上限。本轮没有验证期权合约实际数量，故撤销“ETF 数量已在同一量级”的数据主张。
  - 判定：只要登录后单次订阅集合大于 500，源码就会越界写栈；这是静态可确认的容量缺陷，具体业务负载是否会达到仍未测。建议使用动态容器，并按厂商正式单请求上限分批发送；增加 >500 的边界用例并验证每个合约恰好发送一次。

- **T2-OPT-05 — 文件级静态时间缓冲会在并发调用时竞争；字段长度内不支持原文的常规溢出主张**
  - 源码锚点：ParserCTPOpt.cpp:59-77 定义文件级共享 static char tmp_timestr[10]，strToTime() 对其写入，没有锁或线程隔离；行情时间调用点在 :225-226。TraderCTP 的同名 helper 使用局部 std::string（TraderCTP.cpp:40-54），但属于另一个翻译单元。
  - 证据边界：CTP DepthMarketData 的 ActionDay/UpdateTime 是定长字段，典型合法值不会填满超过 9 个数字；所以目前不能声称正常字段一定缓冲区溢出。若多个 ParserCTPOpt 实例的 API 回调并发执行，则共享 scratch 会互相覆盖；本地测试只验证单线程时间转换（t2_parserctpopt_tick_test.cpp:305），没有并发调度证据。
  - 判定：并发实例下的数据竞争风险成立于调用前提，实际供应商回调并发行为未证；溢出不是正常字段路径的已证问题。建议改为栈上局部数组或直接解析输入，顺带验证空指针/字段长度，并用并发实例测试验证隔离。

- **T2-OPT-06 — “private 面不可达”已被 R105 测试源码部分解决；不是产品缺陷**
  - 状态: 已撤回（private 访问限制是测试可达性问题；未确认独立产品缺陷）
  - 源码/用例锚点：R105 用测试翻译单元的 #define private public 注入 stub（r105_parserctpopt_api_test.cpp:40-42），injected_api_connect_login_and_do_subscribe 覆盖登录成功、缓存订阅和首次动态订阅（:243-281）；login_failure_and_inline_subscribe_branches 覆盖接口可达的登录后订阅分支（:283-317）。T2 的裸构造测试仍覆盖无 API 公开订阅安全路径；不能据此说这些 private 逻辑全部不可达。
  - 证据边界：测试源码中存在这些路径，不代表本轮编译或运行过它们；#define private public 是白盒测试 seam，也不等同于生产 API 扩展。
  - 判定：原“m_pUserAPI、日期、filter、request ID 和上述回调分支均不可达”已过时。没有产品 bug；若仍有具体分支未测，应列明分支与测试前提，而不是笼统把 private 认作缺陷。建议后续优先使用注入 creator/API 的稳定 seam，降低宏替换对头文件的影响。

---

## 复核处置汇总

| 处置 | 编号 | 结论 |
| --- | --- | --- |
| 源码确认缺陷/需修复 | T2-CTP-01、02、03（生命周期部分）、07（订单状态部分）、T2-XELE-01、02、03、T2-OPT-01、03、04 | 源码路径明确；T2-OPT-03 和 T2-XELE-01 有本地定向验证，其余条目未据此声称运行复现，供应商/线程故障仍待验证 |
| 条件风险/接口前提待证 | T2-CTP-04、05、06、08；T2-OPT-02、05 | 静态危险路径存在，但有效输入契约、实际并发/调用前置条件未完全验证 |
| 误报或过时条目应收窄/移出 | T2-CTP-03 原“未连接泄漏”、07 合约状态映射、08 标准适配器重复号断言、09、T2-XELE-02 原“stopped 从未读取”、T2-OPT-02 普通未初始化回调断言、06 | 修正原始陈述；部分编号保留为更窄的真实风险或测试缺口 |
| 覆盖/可测性缺口 | T2-XELE-04；T2-CTP-09 与 T2-OPT-06 的剩余测试维护项 | 不作为产品缺陷，不推断生产转换正确或错误 |
