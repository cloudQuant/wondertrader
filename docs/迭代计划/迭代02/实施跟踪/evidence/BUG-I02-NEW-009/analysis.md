# BUG-I02-NEW-009：WtDtServo 忽略行情解析器初始化失败

## 裁定与证据范围

- 裁定：`PRODUCT_BUG`，状态 `IN_PROGRESS`。
- 根因证据来自 WtDtServo 自己的 `ParserAdapter` 和 `WtDtRunner` 生产调用链静态核查。上一版定向 suite `/tmp/wt-dtservo-status-run.log` 为 28/28 PASS、另有 4 项 disabled，含 parser init 失败后不注册/不启动及 status/sidecar 路径；但其后代码又有修改，更新版本尚未复测，因此该结果不代表当前工作树已通过。
- 本项仅裁定 `src/WtDtServo`；不将结论外推到 WtCore、WtDtCore 或 WtUftCore 的同名实现。

## 源码链路

1. `IParserApi::init` 的接口注释明确返回值表示初始化是否成功：`src/Includes/IParserApi.h:89-95`。
2. `WtDtServo::ParserAdapter::initExt` 保存传入的非空 API 并注册 SPI；底层 `init(NULL)` 返回 false 时只记录错误，函数仍在 `src/WtDtServo/ParserAdapter.cpp:78` 返回 true（`44-79`）。成功才调用 `subscribe`（`58-70`）。
3. 配置式 `init` 同样会在底层 `_parser_api->init(cfg)` 失败时只记录日志，继续到 `src/WtDtServo/ParserAdapter.cpp:202` 无条件返回 true（`149-203`）。
4. `ParserAdapter::run` 只判断 `_parser_api` 是否为空，不验证初始化成功状态，然后调用 `connect()`（`src/WtDtServo/ParserAdapter.cpp:219-225`）。因此 init 失败但 API 指针仍非空时，run 会继续连接失败实例。
5. `WtDtRunner::initParsers` 忽略 `adapter->init(...)` 的返回值，并无条件尝试将 adapter 加入 parser manager（`src/WtDtServo/WtDtRunner.cpp:384-409`）；`start()` 随后运行 manager 中的 parser（`411-420`）。Runner 的正常配置和启动路径因此无法观察底层 parser 初始化失败。
6. R45 的 `InitExtApiInitFailureSkipsSubscribe` 旧断言曾要求 `initExt` 返回成功且 `run` 触发 connect；测试现已调整为断言初始化失败、不订阅、不连接，并验证非空 API 所有权在失败后仍由 adapter 承担。该修正版在上一版 28/28 suite 中通过，但最新代码版本尚未复测。

## 根因和影响

两个 `init` 入口都没有把底层 API 的失败状态映射到自身 bool 返回值，也没有把成功初始化状态作为 `run` 的前置条件。WtDtRunner 又忽略适配器初始化结果并继续注册/启动，所以实际调用者会得到成功表象，失败的 API 仍可能收到 connect 调用。该路径违背 `IParserApi` 的返回值合同，属于产品问题，不只是错误测试。

## 用户已确认的所有权合同

用户确认：`initExt` 收到非空外部 `IParserApi*` 即把所有权转移给 `ParserAdapter`，包括底层 init 返回失败的情况；调用者不因 `initExt` 返回 false 再释放该 API。失败候选由 `WtDtRunner` 保留，直到可验证的安全释放条件满足。此决定只确定本端 owner，不证明 provider stop/release 会排空在途或迟到回调。

## 修复方案

- 两种 `ParserAdapter::init` 均须在底层 API 初始化失败时返回 false；只有初始化成功后才能订阅。
- adapter 应显式跟踪成功初始化状态；`run()` 仅在成功初始化且未停止时调用 connect。
- `WtDtRunner::initParsers` 检查失败，不把失败候选登记到正常运行集合，并将结果传回配置/初始化入口，使调用方可以发现错误；应先暂存并初始化候选，整批成功后再发布到 manager，任何失败都不得部分启动。
- 失败候选可能已注册 SPI 或保留 provider、配置及宿主上下文。`IParserApi::release()` 没有已验证的回调排空合同，所以 runner 在安全释放条件明确前必须保留候选及其借用的 stub/base-data/hot-manager 等上下文，或进入明确的隔离失败状态；不能把 release 返回当成排空证明。
- `initExt` 收到非空外部 API 即转移所有权；该合同包含 init 返回 false 或抛异常路径，调用者不得二次释放。

## 验收条件

1. fake API `init=true` 时，适配器和 WtDtRunner 保持原成功路径：订阅符合当前配置的合约集合，成功注册后可 connect。
2. fake API `init=false` 时，`init` 和 `initExt` 对调用方报告失败，且不订阅、不 connect；runner 不把候选当成有效 parser 启动。
3. 对 API 初始化抛异常的路径验证异常在适配器/runner 边界被安全转换为失败状态；对所有权已转移的非空 API 验证调用者不再释放、adapter/runner 恰好承担一次释放责任。
4. 失败候选及其借用上下文在 provider-specific 安全释放条件得到证明前仍保持有效；测试须验证 owner 保留/错误可见性，不得用无并发 fake 结果推导真实 provider teardown 安全。
5. 保留 R45 的成功 initExt 订阅测试，并将失败测试改为断言失败、无 subscribe/connect；增加至少一个真实 WtDtRunner 注册/启动链路测试。
6. 运行清单对应的定向构建和测试后，再更新此项状态。仅静态代码或 adapter 单测不足以验收 runner 可观察性。

## 当前未验证边界

- 上一版本机 status/sidecar 定向 suite 为 28/28 PASS、4 项 disabled，提供有限的 parser init failure/不注册路径证据；其后有代码修改，当前代码版本没有对应构建/运行记录，故不标 `TEST_PASS`。
- 第三方 parser 的 `release()` 返回后是否还有迟到 SPI 未知，需要 provider 合同或可等待 drain API；不得由所有权转移决策推导已排空。
- parser 回调与失败清理并发时的 provider-specific 停止/排空机制尚未验证，Runner 保留失败候选至可证明的安全释放条件。
