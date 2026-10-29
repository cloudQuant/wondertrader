# T5 lane 缺陷清单（TraderYD / TraderXTP / ParserCTP / ParserShm）

本页按 2026-10-29 对照源码、接口和现有测试复核；仅修改本页，不改生产代码或测试。
用例落点见 `../manifests/T5_yd_xtp_ctp_shm.md`。文中“现状断言”不代表正确契约；该轮静态复核未重新运行测试，
因此动态结论只引用已记录结果，不能当作实盘/XTP/CTP 验收。

---

- ID: T5-01
- 状态: 构造不变量缺失（正常生命周期可补赋值；运行影响待证）
- 位置: `src/TraderYD/TraderYD.cpp:226-239`（构造函数初始化列表）
- 源码事实: 构造函数未初始化这四个字段；`init()` 会设置 `m_bUpdateCache`，`registerSpi()` 会设置 `m_sink`，
  成功登录回调会设置 `m_lDate/m_orderRef`。如果对象在对应生命周期步骤前调用依赖字段的方法，确有读取未初始化值的风险。
- 证据边界: T5 夹具在回调前调用 `init()+registerSpi()`；正常 `TraderAdapter` 生命周期也先注册、初始化再连接。
  本记录没有证明正常生产链在 login 前调用 `makeEntrustID()`，或在 `registerSpi()` 前收到异步回调；
  因而“每次进程随机走分支/正常推单时间错乱”是过强影响描述，当前属于构造不变量缺失与无效生命周期调用风险。
- 用例侧回避: T5 的 YD 夹具在 `SetUp()` 里强制 `init(cfg)`（覆盖 `m_bUpdateCache`）+ `registerSpi(&sink)` 后才驱动回调；
  并**不对** `orderDate/orderTime/委托号自增起点` 下断言。
- 修复方向: 在构造函数给所有原始成员安全默认值（`m_sink=nullptr`、`m_lDate=0`、`m_orderRef{0}`、
  `m_bUpdateCache=false`），并保留 init/login 覆盖。若要主张生产影响，需补一个符合正式调用顺序的 API 用例，
  覆盖“登录前生成委托 ID”是否为允许操作，以及注册/连接回调顺序。

---

- ID: T5-02
- 状态: 产品健壮性候选（空指针路径由源码确认；正常 Adapter 调用前提被原文夸大）
- 位置: `src/TraderXTP/TraderXTP.cpp:901-951`（`queryAccount/queryPositions/queryOrders/queryTrades`）
- 现象: 这四个入口直接 `_api->QueryAsset(...)` / `_api->QueryPosition(...)`，**没有** `_api == NULL` 守卫，
  而同文件的 `login/logout/orderInsert/orderAction`（824/835/844/881）都有守卫；`isConnected()` 也只读状态位。
- 影响与证据边界: 直接在 `_api==NULL` 状态调用四个公开方法会解引用空指针，这由实现可证；
  但 `WtCore::TraderAdapter` 的常规启动查询由登录成功/前序查询回调推进，`queryFund()` 还检查 `AS_ALLREADY`，
  不能据此称 Adapter 的定时查询会在 connect 前无条件调用。缺少厂商库本身并不自动触发查询，
  是否有其他生产调用方在未连接时直调尚未证明。
- 用例侧证据: `DISABLED_query_apis_dereference_null_api_without_guard` 直接在新建、`_api==NULL` 的对象上调用，
  是 API 误用条件的静态边界用例，未运行；不能写作已复现的崩溃。
- 修复方向: 四个入口开头统一检查 `_api`，无 API 时返回错误码（建议 `-1`）并可选记录日志；
  用 stub 分别验证空 API 返回与已连接时请求转发，不把登录后的正常回调链重复作为风险证据。

---

- ID: T5-03
- 状态: 产品候选（空 `entrust` 解引用由代码确认；回调前提与动态崩溃未实测）
- 位置: `src/TraderXTP/TraderXTP.cpp:363-374`（`OnOrderEvent` 错误分支）
- 现象: `makeEntrust(order_info)` 在合约未登记时返回 NULL（`TraderXTP.cpp:199-200`），但调用方既不判空也不换分支：
  `_sink->onRspEntrust(entrust, error)` 先传 NULL，紧接着 `entrust->release()` → 空指针调用；
  且 `_sink` 也未判空（同函数 380 行的正常分支却判了）。
- 影响与边界: 错误回调、`makeEntrust` 找不到合约时会令 `entrust==NULL`，随后无条件 `entrust->release()`；
  若 sink 也为空，`_sink->onRspEntrust` 还会先解引用。源码路径成立，但“换日/新开品种最常见”没有数据支持，
  当前测试条目是禁用用例，本轮未动态复现。有效 `registerSpi` 与 error callback 的厂商触发条件仍需实际 SDK/fixture 验证。
- 用例侧回避: `DISABLED_order_event_error_branch_releases_null_entrust`；启用版 `order_event_with_error_converts_entrust_and_error`
  用已登记合约只覆盖安全面。
- 修复方向: 先验证 `error_info/order_info` 与 sink 的回调契约；`makeEntrust()` 返回空时仅上报可构造的错误对象，
  不释放空指针、不向 sink 传无效 entrust。增加“已注册 sink、有效 SDK 回报结构、未登记合约”的可控 stub 测试，
  分别断言错误回调和不崩溃；不要把空 sink 与空 entrust 混成一个必然生产场景。

---

- ID: T5-04
- 状态: 已合并（BUG-T25-007）
- 位置: `src/Share/WtKVCache.hpp:191-197`（`get`）与 `:198-230`（`put`/`put_if_none`）
- 现象: `get()` 靠 `_indice` 未命中返回 `""`，**不碰** `_cache._block`，所以在未 `init()` 的缓存上是安全的；
  `put()` 则直接 `_cache._block->_size` / `->_capacity` → 未 init 即空指针解引用。
- 影响与边界: `WtKVCache::put/put_if_none` 无 `_cache._block` 判空，直接调用未 init 的实例确会空指针解引用；
  但原文列出的 `orderInsert` 要求登录就绪（YD `WS_ALLREADY`、XTP `TS_ALLREADY`），对应缓存在登录成功路径已 init。
  XTP `makeOrderInfo` 未命中时 `get()` 返回空串并跳过 `_oidCache.put`；非空 usertag 来源也依赖先前映射。
  因此当前证据不支持“这些生产调用在登录前可达”。
- 用例侧证据: 启用用例没有触发未 init 的 `put`；`get` 未命中返回空串的测试不能证明 `put` 安全或不安全。
- 重复关系: `BUG-T25-007` 已有缓存 init 失败后 `put()` 空指针的本机动态证据；与本条“登录前调用未 init 缓存”触发前提不同，
  但底层都是 `_cache._block==NULL` 时写入未守卫。当前不把 T5-04 另计为已证适配器缺陷，后续应并入统一的 `WtKVCache` 空 block 防护验收。
- 修复方向: 可为 `WtKVCache::put/put_if_none` 增加未 init 的安全失败/日志，或以明确 precondition 断言；
  若将本条列为交易适配器线上缺陷，先用调用图或可控状态证明某一实际路径能绕过登录就绪守卫并触发未 init put。

---

- ID: T5-05
- 状态: 条件性候选（负数映射静态成立；负号的厂商语义尚未从本地契约证实）
- 位置: `src/TraderXTP/TraderXTP.cpp:563-566`（`OnQueryAsset`）
- 现象与证据边界: 对负的 `deposit_withdraw` 输入，当前分支确实执行 `setWithdraw(0)`，丢弃负值绝对额；
  本地 XTP 头文件只注释为“当天出入金（仅限期权账户）”，没有说明负值是否代表出金。原测试以 `-8000` 断言现状为 0、
  注释期望 8000，均缺厂商符号契约依据；现有 `query_asset_negative_deposit_withdraw_forwards_account_without_sign_oracle` 移除了金额符号断言，只保留资产回调和总资产检查，与正数对照定向运行 2/2 PASS；厂商动态库和真实回报未验证。其余几个硬置零字段也未在本条提供对应厂商字段映射，
  不能仅因写零就判为缺陷。
- 影响: 只有在 SDK 确认负值表示出金时，出金报表/对账才会受此分支影响；股票账户或字段恒为零的场景不在已证范围。
- 修复方向: 先从当前 XTP SDK 文档或供应商确认符号规则；若负数表示出金，改为 `setWithdraw(-deposit_withdraw)`，
  并在合同确认后补充正/负/零输入的字段映射断言。当前测试只证明负值资产回报仍被转发，不把未经确认的符号解释写成测试预期。

---

- ID: T5-06
- 状态: 静态缺陷候选（自由文本被当格式串；未有带花括号消息的运行证据）
- 位置: `src/TraderXTP/TraderXTP.cpp:360`、`:369`、`:668`、`:762`、`:802` 等（`write_log(_sink, LL_ERROR, error_info->error_msg)`）
- 现象: 厂商返回的自由文本被当作 fmt 的**格式模板**传入（`write_log` 内部 `fmtutil::format(format, args...)`）。
  `fmt::format` 对运行期格式串里的 `{`/`}` 会抛 `fmt::format_error`（或 `on_error` 终止），而这条路径上没有 try/catch
  （`TraderYD.cpp:34-43` 的 write_log 同形，但它至少都传了字面量模板）。
- 影响与证据边界: XTP `write_log` 把 `error_info->error_msg` 直接当格式串；`fmtutil::format`（`src/Share/fmtlib.h:22-28`）标为 `noexcept`，
  若运行时格式解析遇到不配对花括号或无对应实参的占位符而抛错，会触发 `std::terminate`。现有启用测试只用普通文案，
  本轮没有实际注入花括号消息；“柜台文案会含 JSON 花括号”仍是输入假设。
- 修复方向: 把 vendor 文案作为格式化参数传入字面量模板，如 `write_log(..., "{}", error_info->error_msg)`；
  用含 `{`、`}`、`{}` 的消息 stub 断言按原文记录且进程继续。另检查消息指针/空终止符合 SDK 字段契约。

---

- ID: T5-07
- 状态: 产品映射缺陷（API 明确提供 CLOSEYESTERDAY 编码；现有测试锁定了有损现状）
- 位置: `src/TraderXTP/TraderXTP.cpp:118-130`（`wrapOffsetType(XTP_SIDE_TYPE, XTP_POSITION_EFFECT_TYPE)`）
- 现象: 第二个 `else if (XTP_POSITION_EFFECT_CLOSE == offType) return WOT_CLOSE;` 已经吃掉 CLOSE，
  其后 `else if (XTP_POSITION_EFFECT_CLOSE == offType) return WOT_CLOSEYESTERDAY;`（124 行）条件与上面完全相同 → **永不可达**（死代码）。
- 影响与证据边界: 本仓 XTP 头文件定义 `XTP_POSITION_EFFECT_CLOSEYESTERDAY`（值 5）。反向映射重复比较 `CLOSE`，
  因此 CLOSEYESTERDAY 不能回读为 `WOT_CLOSEYESTERDAY`；前向映射也把 `WOT_CLOSEYESTERDAY` 编成普通 CLOSE（值 2）。
  这是可确认的有损映射。现有测试 `offset_type_two_way_and_closeyesterday_alias` 把 CLOSEYESTERDAY 当 CLOSE 的现状锁住，
  不能当作正确契约；还未在真实柜台验证下单侧是否被实际接受/拒绝。
- 修复方向: 前向对照 `XTP_POSITION_EFFECT_CLOSEYESTERDAY`，反向分支比较同一枚举常量并返回 `WOT_CLOSEYESTERDAY`；
  将映射测试拆成 OPEN/CLOSE/CLOSETODAY/CLOSEYESTERDAY 四个独立枚举值，并加从 WTS→XTP→WTS 的往返断言。真实柜台语义另列集成验证。
- 定向验证（2026-10-29）: 已按本仓 SDK 枚举改正并拆分本地映射测试；重建后 `xtp_map.*` 为 7 PASS / 3 FAIL，三条失败均为平昨正向、反向或往返映射。修正了另一条引用号测试对整数回绕后仍为整百的不成立假设；未连接厂商运行时。

---

- ID: T5-08
- 状态: 已合并（主编号 `BUG-R105-004`；T4-010、T2-OPT-03 是同源记录）
- 位置: `src/ParserCTP/ParserCTP.cpp:426-429`（`IsErrorRspInfo`）
- 现象: 形参被 `(CThostFtdcRspInfoField */*pRspInfo*/)` 完全忽略，函数体 `return false;`。
- 复核结论: CTP、CTPMini、CTPOpt、Femas 的 `IsErrorRspInfo` 均为忽略参数并恒返回 false；原文称 Mini/Opt 已正确实现不实。
  这是已登记的跨适配器同源问题 `BUG-R105-004`，此处不单独计数。
- 影响与证据边界: `OnRspError` 不会发布错误；末包登录错误会被当成功并继续访问交易日/订阅。仓库测试只记录空响应的现状、
  `bIsLast=false` 分支或正向 stub，不能证明带非零 ErrorID 的真实 SDK 回调本轮复现。
- 修复方向: 统一用 `pRspInfo != nullptr && pRspInfo->ErrorID != 0` 判错，保持 `NULL` 按无错误响应处理与 SDK 约定一致；
  用 API stub 的非零 ErrorID 覆盖登录、订阅与 `OnRspError`，断言不产生成功事件/订阅请求且错误可诊断。
- 交叉引用: `bugs/R105.md` BUG-R105-004（主条目）；`bugs/T4_femas_ctploader_mini.md` BUG-T4-010、
  `bugs/T2_traderctp_xele_ctpopt.md` T2-OPT-03 为同源重复。

---

- ID: T5-09
- 状态: 待验证（空 API 回调前提未证；与 BUG-R105-004 的有效 API 错误码判定是不同根因）
- 位置: `src/ParserCTP/ParserCTP.cpp:186-206`（`OnRspUserLogin`）
- 现象: 成功分支里 `m_pUserAPI->GetTradingDay()`（190 行）无判空，而同文件 `ReqUserLogin`（368-373）、`DoSubscribeMD`（408）都判了。
- 影响与证据边界: `m_pUserAPI` 确实在 `GetTradingDay()` 前未判空；但 `DISABLED` 用例在全新对象上直接调用厂商回调，
  没有经过 `init/connect`，不证明 SDK 会在无 API 时回调。API `Release()` 后迟到回调的可能性也未从 SDK 生命周期契约或运行日志证明。
  主条目 BUG-R105-004 已包含登录回调的 API 判空建议；此项不单独计数。
- 修复方向: 即使按生命周期不应发生，也可在成功登录处理入口先验证 API 指针并安全记录/返回；
  将测试改为可控 API stub 生命周期（创建后、释放中/释放后分别控制回调），仅在供应商允许的回调时序下定性。

---

- ID: T5-10
- 状态: 待验证（十位数字越界论据已撤回；ParserCTP 的共享缓冲并发影响未证，且不是 ParserCTPOpt 的同一代码路径）
- 位置: `src/ParserCTP/ParserCTP.cpp:59-77`（`strToTime`）
- 源码事实: `strToTime` 把过滤出的数字写入文件级共享 `tmp_timestr[10]`，没有显式限制输入长度；超出 SDK 字段约束时可能越界写。
  多个 `ParserCTP` 实例若在并发回调线程同时调用该函数会共享缓冲并竞争；本轮没有 SDK 并发回调证据或运行复现。
  `constexpr inline` 不会令该函数自动进行常量求值，不单独作为缺陷。
- 重复关系: `ParserCTPOpt` 的同形静态缓冲已登记为 `T2-OPT-05`；R105 也注明 CTP 的 `ParserCTP.cpp:59-77` 同形、未另行登记。
  本条并入 `T2-OPT-05`，只补充 CTP 的字段长度反证与动态证据边界，不单独计数。
- 影响边界: 本地 CTP SDK（`src/API/CTP6.6.9/ThostFtdcUserApiDataType.h:159`）把 `UpdateTime` 放在 `TThostFtdcTimeType[9]` 中；标准时间格式最多 8 字节加终止符，
  因此 `"09:30:15.450"` 不属于这个字段可承载的输入。只有未按 SDK 契约终止的时间字段才可能使读取越界；目前无此运行证据。
- 用例侧证据: `depth_maps_core_fields_into_tick_struct` 使用标准时间字段；它没有覆盖超长/非终止输入或跨实例并发。
- 修复方向: 使用局部固定缓冲并对输入字段长度和终止符做上界检查；并发两实例测试应确认互不覆盖，合法时间和边界输入分别验证。

---

- ID: T5-11
- 状态: 环境/测试范围限制（不是已确认产品缺陷）
- 位置: `src/ParserCTP/ParserCTP.cpp:114-141`（`init`）
- 复核结论: 默认或相对 `flowdir` 配置会在当前工作目录下建立 flow 路径；使用绝对路径时不会在 cwd 建目录。
  `init()` 会调用真实 SDK creator、注册 SPI/front；`connect()` 才调用 SDK `Init()`，可能启动厂商线程。
  这是单测环境的副作用/覆盖边界，不是已确认产品缺陷。非 macOS 的 DLL 加载分支在本机 macOS 构建下不可达。
- 证据边界: 本轮未在真实 SDK 上运行 init/connect/disconnect/release，也未证明其中有生命周期错误；不得写作通过。
- 处置/验证方向: 当前保留 `NOT_RUN`。优先给 creator 增加注入 seam 做 stub 生命周期验证；
  如需真实 init，在明确临时 cwd 下检查目录副作用并确保 release 退出资源，再在非 macOS runner 验证 DLL 分支。

---

- ID: T5-12
- 状态: 构造/配置边界候选（`gpsize=0` 可由合法 init 配置触发；注册前调用属生命周期误用）
- 位置: `src/ParserShm/ParserShm.cpp:63-69`（构造函数）
- 源码事实: `_bd_mgr` 由 `registerSpi` 设置，`_gpsize/_cpu` 由 `init` 设置且构造函数无初值；标准 `ParserAdapter::initExt`
  先 `registerSpi`、再 `init`、随后 `subscribe`，因此“标准接入每次用未初始化值”不成立。`init()` 对显式 `gpsize=0` 不拒绝，
  接收循环在有已订阅数据时执行 `recv_cnt % _gpsize`，此配置可导致整数除零。
- 影响与边界: `subscribe` 在注册前直接调用会读未初始化 `_bd_mgr`，但属于绕过标准注册生命周期的调用；
  真正可独立验证的产品边界是 `gpsize=0`。启用用例没有进入真实接收循环，`DISABLED_` 未运行。
- 修复方向: 构造时给 `_bd_mgr=nullptr`、`_gpsize` 合理默认值、`_cpu` 安全默认值；`init` 拒绝 `gpsize==0` 或归一到正值；
  subscribe/unsubscribe 对缺少 mgr、代码解析错误和索引越界做显式失败处理。用纯内存/mock 接收队列或独立参数测试验证，不需真 shm 才能验证 0 值拒绝。

---

- ID: T5-13
- 状态: 多段标准码解析缺陷有本地替身复现；畸形输入越界为静态风险，真实基础数据注册形状待验证
- 位置: `src/ParserShm/ParserShm.cpp:292-293`（`subscribe`）与 `:306-307`（`unsubscribe`）
- 源码事实: `split` 后不检查段数就访问 `ay[1]`，空字符串或不含分隔符的单段字符串会越界读取；`code` 是 `CodeSet` 中的 `std::string`，
  不是可空 C 字符串，原文“code 不判空”应改为校验空字符串/段数。即使输入恰有两段，当前也只取 `ay[1]`，把其余段丢弃。
- 影响与证据边界: `ParserAdapter::initExt`（`src/WtCore/ParserAdapter.cpp:45-74`）把 `WTSContractInfo::getFullCode()` 放进订阅集；
  `WTSContractInfo::create`（`src/Includes/WTSContractInfo.hpp:171-180`）将原始 code 拼成 `exchg + "." + code`。
  `CodeHelper` 的合法期权码示例（`src/Share/CodeHelper.hpp:131-139`）及测试
  `l01_codehelper_test.cpp:227` 中的 `CFFEX.IO2013.C.4000` 表明系统标准码可包含点。若 `WTSContractInfo` 将该合约 code 保存为
  `IO2013.C.4000`，其完整码会被当前实现拆为 `IO2013@CFFEX`，丢失 `.C.4000`，可能查不到合约或错查；本轮未用真实基础数据管理器确认这一注册形状。
  接口注释没有明文约束 ParserShm 的输入格式。修正后的 `subscribe_and_unsubscribe_preserve_multisegment_standard_code` 注册本地 fake 合约 `IO2013.C.4000`，订阅和退订都实际查找 `IO2013@CFFEX` 而非完整码，定向 FAIL；标准两段码对照 PASS。未用真实基础数据管理器验证注册形状或实盘行情。
- 修复方向: 明确 `IParserApi` 订阅代码格式；若约定为标准全码，应只按第一个 `.` 拆出交易所，并保留后续完整原始 code，拒绝空交易所/空代码/无分隔符输入，
  且在 `subscribe`/`unsubscribe` 共用解析函数。增加单段、空串、多段期权码和普通两段码用例，检查传给 `getContract(code, exchg)` 的完整参数及无效输入的安全失败。

---

- ID: T5-14
- 状态: 原测试构造不成立（fixture 违反 BaseDataMgr 索引/容量不变量）；动态扩容边界待证
- 位置: `src/ParserShm/ParserShm.cpp:297`（配合 `:327`）
- 复核结论: `DISABLED_subscribe_with_zero_global_size_writes_out_of_range` 的 fake manager 报 `getGlobalSize()==0`，同时返回已登记合约且
  手动令 `totalIndex=3`；该状态违反 `WTSBaseDataMgr` 的实现不变量（`WTSBaseDataMgr.h:46`、`WTSBaseDataMgr.cpp:522-523,561-566`）：
  `getGlobalSize()` 是 contract list size，加载时每个合约加入该 list，并在同一列表按 0..N-1 设置 `totalIndex`。
  所以该用例不能证明“零全局槽位却有 index 3”的生产缺陷，原来的确定性越界结论应撤回。
- 证据边界: `registerSpi` 按当前 `getGlobalSize()` 调整位图；`subscribe` 仅当位图为空时再次 resize，而 `unsubscribe` 不 resize。
  如果基础数据管理器允许注册后增加合约，旧位图可能小于新 index；本轮没有找到该运行时动态装载契约或用真实 manager 复现，
  因而该扩容场景仍待验证。普通“没有登记任何合约”的一致状态下，查找不会返回带有效 totalIndex 的合约。
- 用例侧证据: 原 `ZeroSizeShmSink` 人为制造零容量和 index 3，已替换为一致的零容量、无合约启用控制；重建后该控制及两个正常订阅对照共 3/3 PASS。R76 的边界用例是人工扩容路径，不证明 `WTSBaseDataMgr` 支持运行期动态装载；未运行真实 SHM。
- 修复/验证方向: 若基础数据会动态扩容，在 `subscribe`/`unsubscribe` 前按当前 manager 容量扩展位图，并校验 `totalIndex < _subbed.size()`；
  否则明确要求注册 parser 前完成合约表装载。单独确认“注册后合法新增合约”的受支持生命周期；零容量控制不能证明该能力。

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
   进去后 447/484 行 `m_mapPosition->get(key)` 与 448/485 行 `pos->getPrePosition()` **既不判 map 空也不判 pos 空**。
   `m_mapPosition` 由 `notifyFinishInit` 创建，但本轮没有证明厂商会在该回调前送达平仓订单，也没有真实 YD 回调时序证据；
   对已创建的 map，订单关联 key 缺失仍会使 `pos` 为空。相关正常生命周期覆盖与缺失项场景见 **T5-15**，当前只按静态空指针候选记录，不能称本轮崩溃复现。
4. **TraderXTP::connect/reconnect/doLogin**（581-686、753-817）：NULL creator、`fs::create_directories`、真实 Login。

---

- ID: T5-15
- 状态: 产品健壮性缺陷候选（两处空指针路径可静态确认；正常 SDK 时序/丢失持仓场景待验证）
- 位置: `src/TraderYD/TraderYD.cpp:444-474`、`:481-513`
- 源码事实: 首次平仓订单分支和存活订单转撤单分支都直接解引用 `m_mapPosition` 与查找结果 `pos`；前者 map 仅在 `notifyFinishInit` 创建，
  后者还要求订单先存在于 `m_mapOrders`。成交回报的 map 已存在时会为缺失 key 创建 position，但订单回报没有对应保护。缺少 map 或 key 未命中时的空指针路径成立。
- 影响与证据边界: 在 `m_bCatchup=false` 或 `m_bUpdateCache=true` 时进入缓存更新；若收到平仓首报时 map 尚未创建，或 map 已创建但对应 position 不存在，
  会在访问 `pos` 时产生未定义行为。`notifyFinishInit` 在正常路径会创建 map 并装载查询到的持仓；R93 启用用例也在 `login_and_finish()` 后用已登记持仓验证正常冻结/释放，
  没有覆盖 map 缺失、position key 缺失或 SDK 在 `notifyFinishInit` 前回调。故原文“任何一条平仓委托必然崩溃”过强；尚不能认定这些前提在正式 YD 生命周期会发生。
- 重复关系: 与 `BUG-R60-005` 的 `notifyTrade` 空 map/position 路径是相同数据结构但不同回调和代码行，不能合并成一个触发点；R60 文中明确将它与 T5-15 区分。
  R93 只在完成持仓初始化后覆盖正常路径，并将 T5-15 作为既有风险引用，不构成重复缺陷或空指针复现。
- 修复方向: 对 map 先判空或延迟缓存更新到持仓表初始化后；对缺失 key 按成交路径创建带合约/方向的 position，或安全跳过并记录诊断，具体策略需由冻结量契约决定。
  增加 map 未创建、map 存在但 key 缺失、匹配 position 三种 stub 状态测试；向 YD SDK 文档/日志核实 `notifyFinishInit` 与 `notifyOrder` 的先后约束后再定生产影响级别。
