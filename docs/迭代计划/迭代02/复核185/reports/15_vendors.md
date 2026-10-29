# 15_vendors：第二轮核实

本组复核输入中的 15 条 vendor/parser 候选。逐条检查了原始候选、当前实现、仓内 SDK/API、正常调用顺序与测试前置/断言；完整字段和逐条引用见 [triage/15_vendors.json](../triage/15_vendors.json)。

## 分类统计

| 判定 | 数量 | 说明 |
|---|---:|---|
| PRODUCT | 8 | 输入/生命周期可达的代码问题 |
| TEST | 4 | 测试前置或 oracle 不成立，不能据此立产品缺陷 |
| NOT_BUG | 0 |  |
| DUPLICATE | 0 |  |
| EXTERNAL_BLOCKED | 2 | 需要匹配版本的外部 SDK 回调合同 |
| NEEDS_DECISION | 1 | XTP 状态单是否属于产品能力范围 |
| LOCAL_INCOMPLETE | 0 |  |

本轮是静态核实，没有运行测试、构建或真实连接；所有 PRODUCT 仍需按 JSON 的 acceptance 补安全 stub。测试可运行与否不改变此处静态证据等级。

## 逐条结果

### R105

- **BUG-R105-001 — PRODUCT / STATIC。** CTP、CTPMini、Femas 都以 `actHour >= 20` 作为夜盘日期纠偏入口，但保留的 `actHour == 0 && curHour == 23` 分支因此永远不可达。在 ActionDay 等于 TradingDay、行情已到 00:xx 而本地时钟仍为前一日 23:xx 时，代码会把 action date 设成前一日。SDK 结构分开定义 TradingDay 和 ActionDay，源代码注释也表达了系统时钟慢于行情时的预期。应把日期决策改成可注入时钟的纯逻辑，并补三种 parser 的确定性边界测试。现有三项测试只走 23:30 行情并读取真实主机时钟。
- **BUG-R105-005 — EXTERNAL_BLOCKED / PENDING。** Mini、Femas、CTPOpt 在成功登录回调里直接转换 GetTradingDay；空值会令日期维持 0，之后动态订阅只写缓存。仓内 SDK 只说明登录成功后可取得正确交易日，没有给出成功登录时指针、空串和格式保证。CTP 的本地空值 fallback 测试不能外推到其他 SDK，需匹配版本合同。

### R113 / R114

- **BUG-R113-001 — TEST / STATIC。** 常规 TraderAdapter 路径只 connect 一次、只 login 一次，release 才停止并 join；ITraderApi 没有同实例重复调用的重试承诺。禁用测试把第二次 login 和第二次 connect 放在同一个序列里，第二段不可达。撤回“正常生命周期缺陷”主张；若未来支持重试，需先定义状态机并拆开测试。
- **BUG-R113-002 — PRODUCT / STATIC。** 成交路径每次调用 splitVolume 都以秒级 time 重置全局 rand 状态，且模拟撮合线程每 5ms 循环；同秒拆量会反复从相同序列开始并影响进程其他 RNG 使用者。现有长短方向测试只断言总成交量/状态，缺少逐笔拆量与实例隔离断言。建议采用实例内 RNG 并允许固定 seed。
- **BUG-R114-004 — TEST / STATIC。** 被引用的 forwarding 测试先在空 manager 上 run 得到 _live_cnt=0，之后创建了未登记 adapter 再调用终态回调，违反 manager 计数前置。已登记 3 个 adapter 的用例则按 3→2→1→0 递减。修正 fixture 后再判断其他 callback cardinality 问题。

### R76

- **R76-01 — TEST / STATIC。** ParserShm 注册时会解引用 SPI 的 BaseDataMgr，但正常 ParserAdapter::initExt 会注入有效 manager。禁用测试用人工 NULL manager sink，不符合仓内正常初始化路径，也没有接口合同保证 NULL 可用；从产品判定中撤回该崩溃主张。
- **R76-02 — PRODUCT / STATIC。** ParserUDP 在 socket/worker setup 成功后返回 true，却没有调用已有的 doOnConnected；该 helper 会发 Connect/Login，但全仓无调用点。接口提供 SPI 连接事件，当前正向测试只覆盖 disconnect。应在成功 setup 后投递一次就绪事件，并补本地 transport 测试。
- **R76-03 — PRODUCT / STATIC。** ParserXeleSkt 同样在快照与组播 setup 成功后没有调用已有的 doOnConnected。正向成功路径缺少 SPI Connect/Login；现有测试只验证 disconnect-before-connect。应在 setup 成功后通知并用 mock TCP/本地 multicast 验证。
- **R76-08 — PRODUCT / STATIC。** Shm、UDP、Xele 的 disconnect 都将 _stopped 置为 true；connect 不复位该状态。再次 connect 还会 reset 旧的 joinable worker，或复用已停止的 io_context。IParserApi 把 disconnect 与“用于退出”的 release 分开，故同实例断开/重连应是可恢复生命周期。修复必须涵盖 thread join、socket/handler 清理和 io_context 重启；只清 stopped 标志不够。

### T27 XTP

- **BUG-T27-001 — TEST / STATIC。** XTP pinned SDK 将交易日字符串长度定义为 9；测试 fake 的 dash 与 colon 日期均有 10 个字符，而且 SDK 没给出该格式合同。测试把无法由真实 SDK 缓冲容纳的 fake 输入当成成功正向 oracle，不能据此指控解析器产品错。移除这两个断言，先用匹配版本文档确认合法格式。
- **BUG-T27-004 — PRODUCT / STATIC。** Login 失败时先检查 m_sink，再把未检查 m_sink 的 lambda 入队；handler 执行前公开 registerSpi(nullptr) 可注销 sink。现有 seam 用 placeholder worker 和手动 pump 可控制这个顺序，但用例因预期崩溃而禁用。本轮未运行该探针。修复后应把它变成不崩溃的安全回归，并同步 listener 注销与 callback 执行。
- **BUG-T27-007 — NEEDS_DECISION / PENDING。** pinned XTP SDK 有上海新债券 Level2 的 XTP_TBT_STATE，但通用 IParserSpi 没有状态订单回调/记录。需要产品决定支持并定义 WTS 数据模型，或明确暂不支持且增加可观测诊断；enum 存在本身不构成产品能力承诺。

### T3 CTPMini / Dumper / Femas

- **T3-007 — EXTERNAL_BLOCKED / PENDING。** TraderCTPMini 的错误/无行终态路径可能让 m_ayFunds 仍为 NULL，随后末帧无条件 clear。代码路径静态成立，但仓内 `CTPMini1.5.8` 只有 API 头与库，没有错误/零行 NULL/cardinality 规则。SimNow CTPIIMini Ver1.2 与仓内固定版本没有对应声明，不能用近似版本文档定性。修法仍应防御性判空；精确合同确认后再启用安全 stub 回归。
- **T3-012 — PRODUCT / STATIC。** 配置把 refresh_span=0 原样接受；run(false) 在线程中反复 sleep_for(0) 和 refresh，形成无等待循环。当前 smoke test 恰好设置 0 来快速 join，不能覆盖零值政策。应拒绝 0 或把 0 定义成关闭刷新；持续线程测试用正间隔。
- **T3-014 — PRODUCT / STATIC。** 外部 broker/user/pass 以及 CTPMini appid/authcode 从配置进入请求组装；正常 Adapter 还把 user/pass 传入 login。CTPMini/Femas SDK 字段是固定 char 数组，代码未校验便 strcpy 到栈上 SDK 请求结构。按字节校验每字段容量，超限在组装/调用 SDK 前拒绝，不静默截断。现有 CTPMini/Femas stub 只用短凭证，未覆盖 max/max+1；本轮不直接运行溢出探针。

## 未完成的本地动作

本轮未执行任何测试或构建。主代理应按逐条 JSON 的 `local_remaining` 补测试修正、产品 stub 与必要的固定版本 SDK 资料；不要把静态判断表述为动态复现。
