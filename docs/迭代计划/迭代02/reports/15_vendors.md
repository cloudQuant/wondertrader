# 15_vendors 供应商相关候选审查

日期：2026-10-29。输入：`/tmp/wondertrader-review-20261029/groups/15_vendors.json`，共 40 条候选。

## 审查边界

逐条对照了原候选描述、生产实现、公开接口/仓库内 SDK 头文件以及测试前置和断言。此次只做静态审查，没有运行测试、构建、真实网络或柜台调用；因此没有 `LOCAL_REPRO`。JSON 中 `STATIC` 表示源码/合同能支持当前结论，`PENDING` 表示关键产品或供应商合同仍缺，不能把候选文档里的历史运行结果当成本轮证据。

## 裁定数量

| 决策 | 数量 |
|---|---:|
| PRODUCT | 24 |
| TEST | 1 |
| DUPLICATE | 1 |
| UNDECIDED | 14 |
| NOT_BUG | 0 |
| 合计 | 40 |

分 lane：R105 为 4 PRODUCT / 2 UNDECIDED；R113 为 2 UNDECIDED；R114 为 1 PRODUCT / 1 UNDECIDED；R214 为 1 PRODUCT / 1 DUPLICATE；R76 为 3 PRODUCT / 4 UNDECIDED；R93 为 7 PRODUCT；T27 为 2 PRODUCT / 1 TEST / 2 UNDECIDED；T3 为 6 PRODUCT / 3 UNDECIDED。

## 需要主计划吸收的高优先级修法

- 多个可静态确认的数据正确性/交易路径缺陷：R214-005 跨合约撮合；R93-006 平仓冻结与撤单回补；T27-005 深圳逐笔字符映射；T3-008 撤单成功/错误分支反转；R93-003 Femas 查询队列未被三个独立查询入口推进。
- 生命周期/资源：R93-005 XTP worker 无停止与 join；R114-003 TraderAdapter 未调用 deleteTrader 且未卸载模块；T3-009 登录失败后刷新读取未初始化 `_date`；R93-004 disconnect 后状态不清。
- 参数边界/协议：R105-002 固定 500 指针数组越界；R76-06 Xele TCP 快照 EOF 尾数据/整帧遗漏；R76-07 UDP 多合约订阅组包长度偏移造成 NUL 空洞；R76-04 非法组播地址可在 connect 路径抛出异常。
- 配置初始化：R105-003 缺库/符号仍调用空 creator；R105-006 filesystem 异常穿过 bool init；T3-006 缺少配置键及 creator 检查；T3-011 配置对象缺节解引用且忽略 adapter init 失败。

## 需纠正的测试/证据表述

- R105-006 的 `init_flowdir_creation_failure_throws_before_vendor_symbol` 以 `EXPECT_THROW` 为通过条件；`IParserApi::init` 定义为 bool 成功结果。产品缺陷成立，但测试应改为不抛且返回 false，并确认 loader 未调用。
- T27-001 的虚拟 API 测试用 `2026-01-05` 并断言具体损坏整数。SDK 头只说登录成功后可取正确交易日，没有承诺该连字符格式；本条归 `TEST`，应先按供应商确认格式写正常路径，非法格式另测受控拒绝。
- T27-003 的当前测试 `post_login_subscribe_sends_market_data_but_never_tick_by_tick` 明确断言没有逐笔调用；登录初始化路径却对沪深同时发 MD 与 TBT，且 Parser 消费逐笔数据。若按能力一致性修复，该断言应改为对应代码/市场收到 TBT。
- R76-04 的 `InitDefersMulticastValidationUntilConnect` 只断言 init 接受输入，没有调用 connect，也不证明异常/资源泄漏。建议增加不依赖真实网络的地址验证 seam；产品修法应令 init 或 connect 有界返回失败。
- R114-004 所引用的 Mocker forwarding fixture 先对空 manager 调 run，再调用未登记 adapter 的终态回调；这违反计数前置，不能证明生产 over-decrement。是否需 exactly-once 防御仍待 callback 合同确认。
- R93-001 的表达式是 `entrustid + idx + 1`，按 C++ 左结合先算指针加 `npos`，该步即超出原字符串数组；末尾 `+1` 不会回滚。修复应拒绝缺失分隔符并验证数字后缀，避免把结论误写成稳定返回任意值。

## 逐条裁定

| ID | 决策 / 证据级别 | 审查结论与下一步 |
|---|---|---|
| BUG-R105-001 | UNDECIDED / PENDING | 条件静态不可达，但供应商 ActionDay/交易日与跨午夜时钟规则未定；注入固定时钟并先确认规则。 |
| BUG-R105-002 | PRODUCT / STATIC | 三个 parser 的 500 项栈数组无边界；动态容器或合法批次分发，覆盖 500/501 项。 |
| BUG-R105-003 | PRODUCT / STATIC | 缺动态库/creator 时仍调用空指针；校验所有加载层并通过 fake loader 验证失败返回。 |
| BUG-R105-004 | PRODUCT / STATIC | 四处错误判断恒 false，登录错误仍走成功通知/订阅；保留非零错误断言并覆盖各实现。 |
| BUG-R105-005 | UNDECIDED / PENDING | 空 GetTradingDay 会锁住动态订阅，但 Mini/Femas/Opt 的 SDK 没保证空值可达；逐厂商确认后决定回退/重试。 |
| BUG-R105-006 | PRODUCT / STATIC | create_directories 异常逃出 bool init；用 error_code/异常捕获并返回 false，修正 EXPECT_THROW。 |
| BUG-R113-001 | UNDECIDED / PENDING | 重复 connect/login 可销毁 joinable thread，但重复调用是否在支持生命周期中未定义；分拆测试并定状态机。 |
| BUG-R113-002 | UNDECIDED / PENDING | 全局 PRNG 被重置是事实，确定性回放契约未找到；确认产品要求后再用实例 RNG。 |
| BUG-R114-003 | PRODUCT / STATIC | API 对象、DLL handle 都未按导出删除协议回收；保留 remover/handle 并测试删除与 unload 次数。 |
| BUG-R114-004 | UNDECIDED / PENDING | 0 下溢静态成立，但引用 fixture 的 adapter 未登记且计数为 0；重复终态回调合同待确认。 |
| BUG-R214-004 | DUPLICATE / STATIC | 与 `BUG-R60-001` 同是 disconnect 后 release 早退漏停 io worker；归并到 canonical。 |
| BUG-R214-005 | PRODUCT / STATIC | 撮合未核对订单自身合约；保留跨合约隔离、同合约正控和零量回归。 |
| R76-01 | UNDECIDED / PENDING | NULL base manager 会被解引用，但接口没有 nullable 约定；先确定 `getBaseDataMgr` 前置。 |
| R76-02 | UNDECIDED / PENDING | ParserUDP 的连接事件函数无调用者，但接口未承诺 connect 成功回调；先统一事件合同。 |
| R76-03 | UNDECIDED / PENDING | ParserXeleSkt 同型，断开测试未证明缺连接事件违约；先统一事件合同。 |
| R76-04 | PRODUCT / STATIC | 非法 mcast/local 地址会在 connect 中抛异常，且 socket 已开始创建；校验地址并将失败变成 bool 结果。 |
| R76-06 | PRODUCT / STATIC | 固定长度 read 在 EOF 时可能丢已读尾字节，`>` 也留下一整帧；抽出解析 seam 测 1/N 帧及截断。 |
| R76-07 | PRODUCT / STATIC | UDP 组包只写代码后缀却按完整码长度前进；修正偏移并在写前分包。 |
| R76-08 | UNDECIDED / PENDING | 三个 parser disconnect 后不复位 stopped，但同实例重连契约未找到；先定生命周期再补整链测试。 |
| BUG-R93-001 | PRODUCT / STATIC | 缺 `#` 时 `idx==npos`，左结合指针加法越界；在解析前验证分隔符和纯数字后缀。 |
| BUG-R93-002 | PRODUCT / STATIC | YD queryOrders/queryTrades 的数组和元素引用未 release；回调后释放并测引用归还。 |
| BUG-R93-003 | PRODUCT / STATIC | Femas 三个查询只入队不调用 triggerQuery；从公开入口验证 stub 请求调度。 |
| BUG-R93-004 | PRODUCT / STATIC | XTP disconnect/release 不清 `_state`，isConnected 返回过期状态；补状态迁移测试。 |
| BUG-R93-005 | PRODUCT / STATIC | XTP worker 无限循环且 release/destructor 不 stop/join；实现停止与 join 后测真实受控 worker。 |
| BUG-R93-006 | PRODUCT / STATIC | OpenCover 冻结比较恒假、撤单来源反推错误；记录每单冻结今昨来源并测混合/部分成交。 |
| BUG-R93-007 | PRODUCT / STATIC | 构造函数漏初始化 `_state/_sessionid`；加确定默认态并测试构造初态/未登录 logout。 |
| BUG-T27-001 | TEST / STATIC | 假日期格式未获 SDK 认可，测试硬断言损坏值；先校验供应商格式，坏输入应单独拒绝。 |
| BUG-T27-003 | PRODUCT / STATIC | 动态订阅缺少初始化路径已有的 TickByTick 调用；更新沪深两市场断言。 |
| BUG-T27-004 | UNDECIDED / PENDING | pending lambda 解引用已 detach 的 sink 是静态路径，但 detach/队列生命周期合同不明。 |
| BUG-T27-005 | PRODUCT / STATIC | SZ 字符直接强转为 WTS enum 导致方向/类型错映射；按 SDK 字符表逐项映射。 |
| BUG-T27-007 | UNDECIDED / PENDING | SDK 有 XTP_TBT_STATE，但 Parser 是否承诺支持、WTS 如何表达未定；先定能力边界。 |
| T3-004 | PRODUCT / STATIC | Femas 无 API 时仍报告命令已发送，连接态只看 API 指针；实现连接状态机；退订不纳入。 |
| T3-006 | PRODUCT / STATIC | Mini trader 缺键空解引用、creator 未判空；分开测配置与 fake loader 失败。 |
| T3-007 | UNDECIDED / PENDING | 空末帧可能对 NULL 数组调用 clear，但 CTP 零行/错误回调契约未确认；源码层可判空防御。 |
| T3-008 | PRODUCT / STATIC | 非空撤单回报错误分支反转；NULL pointer 契约另议，保留成功/失败两条回归。 |
| T3-009 | PRODUCT / STATIC | 登录失败后 done adapter 被 refresh 查询，先读未初始化 `_date`；初始化为 0 并测试刷新不查询。 |
| T3-011 | PRODUCT / STATIC | Dumper 缺 root/basefiles/traders 解引用且忽略 adapter init 结果；验证 false 返回、不登记失败通道。 |
| T3-012 | UNDECIDED / PENDING | refresh_span=0 会空转，但零值是禁用还是非法未定；先明确配置语义。 |
| T3-013 | PRODUCT / STATIC | 255 字节 getcwd buffer 忽略失败，cwd 深时读取未写入缓冲；改动态/错误码路径并测深目录。 |
| T3-014 | UNDECIDED / PENDING | 定长字段 strcpy 超容量可溢出，但超长配置是否属于受支持输入无 schema；确认字段限值后边界拒绝。 |

完整字段、证据路径、逐条修法与验收条件见 [triage/15_vendors.json](../triage/15_vendors.json)。
