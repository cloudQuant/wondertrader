# 03_core_orders：185 条第二轮核实

## 结论

本组复核 13 条：PRODUCT 6 条，TEST 2 条，NOT_BUG 1 条，DUPLICATE 1 条，NEEDS_DECISION 2 条，EXTERNAL_BLOCKED 1 条。仅 BUG-P01-001 与 BUG-R15-005 的共同缺 session 输入前提由本轮定向执行确认可由基础数据加载器产生；本轮没有运行任何空指针崩溃测试。其余结论依据生产调用路径、接口注释、状态写入来源与现有测试前提作静态裁定。

本轮单独运行 R56BaseDataMgr.ContractRulesAutoCreateCommodityAndWindows，命令为 /Users/yunjinqi/opt/anaconda3/bin/conda run -n base python docs/迭代计划/迭代02/复核185/evidence/03_core_orders/run_r56_case.py。实际 GTest filter 仅该用例，结果 1/1 PASS，目标 SHA-256 为 50e6a8019a6192cff4ceaee707dd8886181e5b19a97070a13d4a06b082c063e3。它验证 rules 自动建品种可以在 ALLDAY 模板缺失时仍成功，并把 sessionInfo 设为 NULL；没有执行 CTA/SEL 的 session 解引用路径，故相关缺陷为“输入可达性本地复现 + 崩溃路径静态确认”，不是崩溃复现。原始命令、stdout、stderr、XML 和运行元数据见 evidence/03_core_orders/。

明确测试修改请求已发给主代理，涉及 active 风控缺 policy、Parser API 初始化失败传播、CTA 缺 session 正常数据路径、CTA 未知 commodity 的 set-position 副作用，以及 CTA/SEL 持久化 schema 负例。没有修改任何生产源码、测试源码、构建文件或旧报告。

## 逐项结果

### BUG-R15-002 — NOT_BUG（当前零容量路径不可独立观察）

_orders 默认构造为零容量 boost::circular_buffer；生产上下文构造时未设置容量，正常 HFT 下单确实会调用 setUserTag，但 Boost 对零容量容器的 push_back 不保存元素。因此当前 getOrderTag 总是空、eraseOrderTag 总是无操作，lower_bound 的缺键后继匹配/误删只有在 R15-001 把容量启用后才可观察。见 src/WtCore/HftStraBaseCtx.cpp:41-48,234-236,255-257,298-300,318-320、src/WtCore/HftStraBaseCtx.h:625-668,774 和 Boost circular_buffer 构造注释 build-cov/TestUnits/coverage/cov_boost_include/boost/circular_buffer/base.hpp:1008-1025。

现有 DISABLED_order_tag_lookup_miss_does_not_match_or_erase_neighbor 写的是正确的精确 ID 语义，但它明确说明缺少建立非空容器的生产 seam；目前不能据此单独登记第二个运行时缺陷。若修复 BUG-R15-001 并设置容量，应将“中间缺失 ID 查询为空、删除不碰后继”的断言纳入同一回归。

### BUG-R15-003 — PRODUCT

SEL 正常 on_init 会读取本地策略持久化文件。语法错误/空文件有早退，但 JSON 只要语法正确，loader 就会直接访问 root["positions"]、fund/position/detail/signal 的字段并调用 RapidJSON Get*；用户数据加载也对值直接 GetString。只有 opentdate/maxprice/minprice 等少数字段有兼容守卫。save_data 写出规范字段，但没有 schema/version 检查或“先校验、后更新”步骤。见 src/WtCore/SelStraContext.cpp:16-21、src/WtCore/SelStraBaseCtx.cpp:198-227,229-377,379-460,502-510。

r15_sel_persist.cpp 覆盖完整信号恢复/落盘与语法错误、空文件早退，没有语法正确但缺字段、错误类型或非 object 根的文件。持久化快照是每次策略初始化会读入的本地文件边界；损坏、跨版本或部分写入的状态不能使进程落入 RapidJSON 断言，也不能在字段读到一半后留下局部状态。修复需为快照定义版本与必需 schema，先整体验证，再整体应用；失败时保留默认/既有状态并给出日志。验收需覆盖完整 round-trip、支持的旧版本迁移及损坏 schema 整体拒绝且无状态半更新。

### BUG-R15-005 — DUPLICATE（canonical：BUG-P01-001）

SEL 的独立消费者路径成立：文件恢复的待发信号会在 on_tick 调用 get_session_info，随后直接调用 isInTradingTime；WtEngine::get_session_info(stdCode,true) 在 commodity 不存在或 session 未解析时返回 NULL。见 src/WtCore/SelStraBaseCtx.cpp:348-375,555-571、src/WtCore/WtEngine.cpp:104-115。有效 session 的信号恢复测试只验证“非交易时段保留信号”，不覆盖缺 session。

与本组 BUG-P01-001 共享的根输入已由本轮执行 R56 loader 测试确认：基础数据加载器对合同 rules 使用 ALLDAY 默认值，即便该模板未加载仍插入 commodity，且用例断言其 getSessionInfo()==NULL。因此本条作为同一基础数据缺 session 接受/消费问题并入 CTA 主编号 BUG-P01-001，canonical 只指向该主记录，不反向循环。主记录的验收需同时覆盖 SEL 恢复信号后的 tick；保留正常 session 下“非交易时段信号不消费”的原语义。

### BUG-N01-004 — PRODUCT

交易通道配置由 WtRunner::initTraders 逐项传入 TraderAdapter::init；riskmon.active=true 且未提供 policy 时，init 在 cfgPolicy->memberNames() 前没有检查空指针。它先 retain _cfg 并写入部分 adapter 状态；空 policy 会在配置装载边界直接解引用，WtRunner 也忽略 init 的返回值并继续注册 adapter。见 src/WtCore/TraderAdapter.cpp:99-165、src/WtRunner/WtRunner.cpp:551-575。现有 enabled 测试只证明 active=false/无 riskmon 会短路，不覆盖 active 缺 policy。

修复应在保留/提交 adapter 状态前验证 riskmon/policy 类型和必需成员；active 风控缺 policy 时明确初始化失败，不得改成空规则继续交易，并让 Runner 不注册初始化失败的通道。新增用例应包括 active=false、active=true 完整 policy、active=true 缺 policy；后者断言 init 和 Runner 配置安全失败，不崩溃也不进入 run。

### BUG-N01-006 — PRODUCT（收窄到 API 初始化失败传播）

原候选把三个 Level2 handler 对人工 NULL payload/未初始化对象的解引用列为问题，但接口没有声明回调允许 NULL，禁用用例只调用 handleQuote，没有触发 Level2 handler。该部分不能单独证明产品缺陷。独立可达的产品问题是 IParserApi::init 注释明确以 bool 表示初始化成功与否，而 ParserAdapter::init、initExt 在 API 返回 false 时仅记录日志，最终仍返回 true；WtRunner::initParsers 忽略 adapter init 返回值并仍登记 adapter，随后 run() 会调用 connect()。见 src/Includes/IParserApi.h:89-95、src/WtCore/ParserAdapter.cpp:45-82,156-211,228-235、src/WtRunner/WtRunner.cpp:428-458。

修复应从 ParserAdapter 传播 API 初始化失败、校验成功初始化所需的基础数据 manager，并让 Runner 只登记成功初始化的 adapter。新增 failed-API-init 用例应使用有效 fake manager、init=false 的 fake API，断言 init 返回 false、没有订阅/连接，也不进入 Runner adapter 表；成功 API 的完整合同订阅路径保持不变。现有 disabled quote 测试不作为本条动态证据。

### BUG-P01-001 — PRODUCT（session 缺失的可达性已本地确认）

CTA 的 pending-signal on_tick 在 _sig_map 命中后取 session 并直接调用 isInTradingTime；stra_exit_long 也在读取持仓前直接对 commodity 的 session 调 offsetTime/getCloseTime。WtEngine::get_session_info 在 session 未解析时返回 NULL。更关键的是本轮真实执行的 R56BaseDataMgr.ContractRulesAutoCreateCommodityAndWindows 加载成功并产生 commodity!=NULL && sessionInfo==NULL，不是测试子类人工造空指针。CTA 可经公开 stra_set_position 创建待处理信号并订阅 tick，或经 stra_exit_long 进入平仓路径。见 src/WtCore/CtaStraBaseCtx.cpp:795-817,1229-1244,1352-1409、src/WtCore/WtEngine.cpp:104-115、src/WTSTools/WTSBaseDataMgr.cpp:429-448、src/TestUnits/coverage/l02_wtstools_mq/r56_basedatamgr_test.cpp:68-86。

原 DISABLED_P01CtaCrashClass 使用假管理器造缺 session，且第一条 stra_exit_long 若崩溃，后面的测试语句均不可达；它不代表正常数据加载链。但 R56 已证明正常加载入口会成功返回缺 session commodity，故源边界缺少失败保护仍是产品问题。建议在 stra_exit_long 和 pending-signal tick 两处对 session 缺失做可诊断拒绝/安全返回；tick 路径不得错误消费待发信号。验收需用真实 loader fixture 分别覆盖这两处，验证不崩溃、不提交错误交易/持仓变化，信号状态明确，并确认有效 session 的既有交易窗口语义不变。R56 运行只证明输入生成，不是崩溃测试。

### BUG-P01-002 — TEST

历史查询的候选来自裸 P01CtaEngine::attach fixture：它只设置 _base_data_mgr/_hot_mgr 和时钟，不调用 WtEngine::init；WtEngine 构造函数未初始化 _data_mgr，正常 init 才赋值。禁用测试在历史查询语句之前先调用会因缺 session 解引用的 stra_exit_long，因此历史查询那几行根本不可达。见 src/TestUnits/coverage/p01_wtcore_stractx/p01_cta_env.h:224-250、src/TestUnits/coverage/p01_wtcore_stractx/p01_cta_order_cond_test.cpp:270-290、src/WtCore/WtEngine.cpp:39-56,285-302,592-641、src/WtCore/WtEngine.h:206-209。

Runner 正常路径先 initEngine，再 initDataMgr，然后才创建策略/context；engine init 会保存数据管理器指针，策略回调在此初始化之后运行。见 src/WtRunner/WtRunner.cpp:153-160,275-280,365-408。现有测试因此把“违反初始化生命周期的裸 engine”误标为普通产品崩溃。该项不要求产品改动；若保留测试，应将历史查询从混合 DISABLED 体拆出，使用已初始化的数据管理器验证正常查询。init 前行为目前没有公开承诺，不应为其虚构崩溃回归。

### BUG-P01-003 — PRODUCT

ICtaStraCtx::stra_set_position 的公开描述是为指定合约设置目标仓位；但对不存在 commodity 的 stdCode，stra_set_position 立即订阅并追加/持久化信号或条件单，直到后续 do_set_position 才发现 commodity 为空并返回。对照 stra_enter_long 在订阅前先检查 commodity。这里仅判定“commodity 不存在”的输入，不把已知商品下的未知月份合约扩大成缺陷。见 src/Includes/ICtaStraCtx.h:241-252、src/WtCore/CtaStraBaseCtx.cpp:1119-1128,1352-1425。

缺少前置验证会造成无效代码订阅与持久化状态，后续正常处理不能下单却可能长期保留条件/订阅。修复应在任何订阅、信号、持久化副作用前执行与其它 CTA 下单入口一致的 commodity 检查。新增用例应以不存在的 commodity 调用 stra_set_position，断言无 tick subscription、signal、condition 或 JSON 写入；有效 commodity 路径保持现有行为。

### BUG-P01-004 — PRODUCT

Cta context 正常 on_init 会读 stradata/<strategy>.json。语法错误有早退，但 parse 成功后对 root["positions"]、必需 position/detail/condition 字段直接调用 Get*，在 root 缺成员、类型错误或结构错误时没有完整 schema 校验；而 save_data 写出完整的当前字段。见 src/WtCore/CtaStraBaseCtx.cpp:277-430,495-558,668-676、src/WtCore/CtaStraContext.cpp:64-71。r42_cta_persist_test.cpp 仅覆盖完整有效快照和 round-trip，没有缺字段/错类型 schema fixture。

策略本地持久化文件会由正常初始化自动加载；损坏或旧版本文件不应令启动断言终止，也不应边读边写造成部分仓位/条件状态。修复需定义版本，先完整校验整个快照，验证通过后再应用；失败则安全拒绝并保留默认状态。验收需涵盖完整当前 schema、明确支持的旧 schema、缺字段/错误类型以及无部分状态污染。

### BUG-P11-001 — NEEDS_DECISION

initExt 初始化外部 API 但不设置 _cfg；run() 注册 SPI 后调用 connect()；成功 WTE_Connect 事件则无条件读取 _cfg->getCString("user/pass") 并调 API login。ITraderApi 将 connect 与 login 分开，但没有说明 initExt 的认证所有权；内置 latency 工具只传入 fake API，当前禁用测试则直接调用 handler，未走真实 connect 回调链。见 src/WtCore/TraderAdapter.cpp:209-235,404-416,1405-1417、src/Includes/ITraderApi.h:198-237、src/WtCore/TraderAdapter.h:167-175、src/WtLatencyHFT/HftLatencyTool.cpp:461-475、src/WtLatencyUFT/UftLatencyTool.cpp:288-300、src/TestUnits/coverage/p11_wtcore_traderadapter/p11_crash_class_test.cpp:95-118。

需由 API/adapter 接口所有者明确：initExt 的 API 自己负责认证，Adapter 不再自动 login；还是 Adapter 负责认证并需由 initExt 接收凭据。建议统一为“凭据来源显式、连接成功只登录一次”，优先扩展 initExt 明确传入配置/凭据并保留内部 API 与外部 API 的兼容层。选择会影响现有外部注入实现是否已自登录，以及现有调用方是否依赖 Adapter 登录。决定前不补模糊的 handler 直调测试。

### BUG-P11-002 — TEST（当前 crash-class 输入没有产品可达性证据）

候选中的空数组容器本身不是问题：Adapter 对整个 NULL 数组已有守卫，OES 在零结果时会传 NULL；当前 enabled R59Misc.null_report_arrays_advance_state_machine 覆盖该路径。候选所指的 NULL 数组元素、NULL push-trade、NULL error 则由 disabled crash-class 用例人工构造，且该用例先触发 P11-001 连接分支。已查看的 OES/XTP provider 路径在转换结果非空时才 append/推送对象，错误路径则创建 WTSError；ITraderApi 的注释没有描述空 payload 合同。见 src/WtCore/TraderAdapter.cpp:303-389,1514-1533,1739-1751,2148-2154,2337-2344、src/Includes/ITraderApi.h:170-180、src/TraderOES/TraderOES.cpp:481-483,985-1000,763-778、src/TraderXTP/TraderXTP.cpp:388-397,552-575、src/TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_misc_test.cpp:70-105、src/TestUnits/coverage/p11_wtcore_traderadapter/p11_crash_class_test.cpp:95-125。

因此本候选当前只能撤回为“无已证明的正常 provider 空元素/对象路径”，不能要求产品对所有人工 NULL 值统一静默；也不据此宣称所有第三方插件均安全。若 API 将来正式允许 NULL，按每个 callback 补清晰合同与分支测试。注意 onRspEntrust(entrust,NULL) 是 HTS/DD 的成功回报且只进入 err 条件为真的失败分支判断，不是本条空错误对象崩溃证据。

### BUG-P11-005 — EXTERNAL_BLOCKED（doCancel 子主张不成立；XTP 错单回调合同待核）

doCancel 使用 adapter 活单 map 中订单；onRspOrders/onPushOrder 入表前检查 contract，因此裸对象直接 doCancel 不能代表正常取消路径。另一子路径需保留：XTP makeEntrust 在 contract lookup 失败时返回 NULL，OnOrderEvent 错单分支随后无条件把它传给 onRspEntrust 并调用 entrust->release()；adapter 的 error 分支也会直接读取 entrust contract。见 src/WtCore/TraderAdapter.cpp:1244-1268,1450-1464,1604-1623,1873-1882、src/TraderXTP/TraderXTP.cpp:190-200,363-374。

缺少 contract 的 callback 是否是当前 XTP SDK 合法事件尚不能由 stub 确定：正常 Adapter 下单先检查合约，SDK 报告应能否只回报该已提交订单、是否保证 ticker/market 可映射到当前基础数据，都需要该版本 SDK 合同。已检查本地正常下单与映射代码和 XTP 包装层；人工构造无 contract WTSEntrust 的 disabled 测试不提供真实 SDK 回报语义。需取得确切 XTP SDK 文档/源代码条款或真实无写入 callback 记录后再决定是否对 callback 增加 NULL 处理，并拆分 doCancel 与 onRspEntrust 的测试。

### BUG-P11-007 — NEEDS_DECISION

initExt 无条件 _save_data=true 并创建 getBaseDir()/traders/<id>/ 下的 trades/orders CSV；账户回报还会生成 rtdata snapshot。HFT/UFT latency 工具用 initExt 装配注入 API 时也会触发该副作用。现有 enabled P11PushPersist.init_ext_creates_csv_headers 把写文件作为当前行为断言，但它不能回答这是否是所有外部 adapter 的产品要求。见 src/WtCore/TraderAdapter.cpp:209-235,237-278,303-389、src/WtLatencyHFT/HftLatencyTool.cpp:461-475、src/WtLatencyUFT/UftLatencyTool.cpp:288-300、src/TestUnits/coverage/p11_wtcore_traderadapter/p11_push_persist_test.cpp:117-130。

请接口所有者决定 initExt 落盘策略：继续默认开启并文档化；改为默认关闭、调用者显式 opt-in；或新增 savedata/输出目录参数。建议第三种并为当前版本保留兼容默认，后续由配置显式迁移。影响包括既有 CSV/快照使用方、latency 工具文件副作用以及测试清理路径。决定后再补启用/关闭/自定义目录、多 adapter 不互相覆盖的测试。

## 待闭环

本轮未构建、未运行全量测试或已知会触发 UB 的 disabled 测试。6 条产品项应先按 triage 的具体测试请求与实现验收闭环；BUG-P11-001、BUG-P11-007 等接口选择由责任人明确后再补对应测试；BUG-P11-005 需先取得 XTP SDK 回报合同。R56 的 PASS 只证明 session 缺失输入可由 loader 接受，不证明 CTA/SEL 崩溃已经动态复现。

