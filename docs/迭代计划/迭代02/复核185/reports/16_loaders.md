# 16_loaders：五项复核

## 结论

5 项中，**PRODUCT 3 项、NOT_BUG 2 项**；五项均为 STATIC。本轮没有运行任何禁用的挂起/UB 用例，也没有连接真实柜台或加载真实厂商库。复核同时确认了 API 入口、SDK 头文件、实际调用方及有效/无效 fixture 的区别。

刷新后的 CoverageTests_t_CTPLoader 与 CoverageTests_t_TraderFemas 使用独立进程、临时工作目录、LLVM profile 和 GTest XML，运行时限 15 秒。5 个 enabled stub 用例中 4 个通过；唯一失败是已有 fee fallback 用例的相对输出文件名断言，与本组 T26-007 的缺费迭代器主张不同：测试期望 custom_fees.json，实现仍把相对 FEES_FILE 固定映射成 fees.json（TraderSpi.cpp:713-720，对应 T26-006）。完整 stdout、stderr、XML、SHA-256、退出码和准确测试数见 evidence/16_loaders/safe_stub_runs.json。

## 逐项判断

### BUG-T26-001 — NOT_BUG：worker 析构挂起不在当前 loader 生命周期中触发

AppendQuery 的 worker 在空队列内层循环不看 _stopped，析构置停后 join() 确有挂起风险。但导出入口 CTPLoader::run 以 new 创建 SPI 并注册给 API，随后只调用 Init() 和可选 Join()；可见路径没有保存 owner、delete pUserSpi 或 Release API 的关闭路径。回调终局通过 exit(0) 结束进程。现有 disabled 用例额外手工 delete，未证明产品会走这一步。

本轮 login_success_writes_session_refs_and_queues_instrument_query 使用 CTP stub 通过，确认正常查询能启动 worker；它没有执行析构，不能作为挂起复现。当前应撤回“正常 loader 生命周期卡在 SPI 析构”的主张。loader 缺少资源回收/正式 stop 路径是相邻问题，本条不据此断言资源生命周期完整。

### BUG-T26-002 — PRODUCT：外部 CZCE 合约字段缺少有界验证

fillCzceCode 在 for(;;) 中持续读到数字才结束；回调收到的 InstrumentID 和期权 UnderlyingInstrID 在 CZCE 分支直接传入。有效 CZCE 合约形状应维持现有转换；但来自 SDK 的固定长度字段若空、没有数字或没有终止符，当前扫描会读出字段范围。

原 disabled 测试直接以 "SRCS" 调 helper，格式不是有效 CZCE 合约，因此不是合法合约输入的反例。本轮有效 SR510 -> SR2510 回调测试通过，证明正常形状仍可作对照；它不覆盖畸形回报。修复应对 SDK 字段容量做有界解析，拒绝无数字/未终止内容，并保留期货、期权及 underlying 的既有输出。

### BUG-T26-007 — PRODUCT：佣金回报缺失可令 fee lookup 解引用 end

费率查询从 _contracts 中逐合约排队。佣金错误回报不会写入 _fees，但回报处理仍会在 _queries.empty() 时调用 DumpFees()；保证金回报也会在相同条件下调用它。DumpFees 遍历 _margins，先按合约查 _fees，再按品种 fallback；两个查找都 miss 后直接取 it->second。因此只要有效合约有保证金回报、其佣金结果未登记且无品种兜底，就会走到未定义行为。这里的正常输入来自已登记合约和 SDK 回调，不要求手工往内部 map 塞未知码。

原 disabled 用例只写 _margins[ZZ99]，既没有 _contracts 记录也没有回调生命周期，夹具前置不成立；不要启用。修复应使用 find 检查合约与 fee 两级结果，缺费时明确记录并跳过或写缺失标记。正向 fallback 用例本轮进入 dump，但在文件路径断言处失败于独立的 T26-006 相对路径行为；不能据此声称 T26-007 已复现或通过。

### BUG-T4-003 — PRODUCT：Femas Init() 与 strand 初始化顺序存在回调竞态

connect() 在注册 API 后先调用 vendor Init()，然后才创建 m_strandIO。SDK 头文件只说明 Init() 调用后 API 开始工作，没有承诺回调要等 Init() 返回。登录完成会同步调用 onInitialized() 和 sink；若 sink 在成功登录通知中重入 queryAccount()，它会进入 triggerQuery() 并解引用尚未创建的 strand。

当前 WtCore TraderAdapter::onLoginResult 只调用 queryPositions()，不是该具体重入路径；这不消除公共 ITraderSpi callback 由调用方重入查询的可能。旧 disabled 用例通过 sentinel API 和手工 WS_ALLREADY 绕过 connect()，不是有效复现。修复方向是 API Init() 前构造执行器，或在执行器 ready 前缓存查询；测试应由 fake API 的 Init() 同步触发成功回调，并在 sink 内重入查询。

### BUG-T4-004 — NOT_BUG：无效 sentinel 不能证明成功登录时 API 缺失

正常 connect() 先创建并保存 API 指针，再注册 SPI 并调用 Init()。因此 API 在有效登录成功回调路径上已存在；原 disabled 测试设置 kApiSentinel=0x1 后直接调用成功分支，既不是 null API，也不是可访问的厂商实例。仓库已有 R93FemasApiStub 实现 GetTradingDay()；启用的 fake login 测试本轮通过，成功分支无需真实动态库。

撤回原“成功登录但无 API”的产品主张，移除 sentinel 负例并复用 fake API 正向覆盖。Femas 头文件没有说明 RegisterSpi(NULL)/Release() 对在途 callback 的静默与等待保证；这份结论不代表并发 release 与 callback 的生命周期已经验收，若继续追查应作为独立 shutdown 问题核对具体 SDK 条款。

## 本轮安全 stub 执行

| Target / filter | 结果 | 说明 |
|---|---:|---|
| CoverageTests_t_CTPLoader / t26_ctploader_flow.login_success_writes_session_refs_and_queues_instrument_query | PASS 1/1 | Stub 队列正常启动并发出查询；未销毁 SPI。 |
| CoverageTests_t_CTPLoader / t26_ctploader_instrument.czce_future_code_is_expanded_and_altcode_kept | PASS 1/1 | 有效 CZCE 期货回调与 SR510 扩年对照。 |
| CoverageTests_t_CTPLoader / t26_ctploader_dump.margin_response_dumps_fees_to_configured_relative_name_with_product_fallback | FAIL 0/1 | t26_ctploader_dump_test.cpp:259 找不到 custom_fees.json；实现实际落到 fees.json，属 T26-006 路径行为，不是缺费 lookup 复现。 |
| CoverageTests_t_TraderFemas / R93FemasTest.authenticate_login_and_query_front_reach_ready | PASS 1/1 | Fake API 正常登录成功分支及 trading day。 |
| CoverageTests_t_TraderFemas / R93FemasTest.connect_and_release_drive_stub_lifecycle | PASS 1/1 | Fake API connect/register/release 顺序；未模拟 Init 内同步回调。 |

测试目录 t26_ctploader_common.hpp:192-200 会先删除固定临时目录 wtdump_<case>；本轮所选三个 CTP case 分别使用 f4、i13、p3。Femas case 使用 PID/序号目录。运行输出和 profile 保存在 evidence/16_loaders/。
