# 06_mockers 候选审计

## 范围与证据界限

本轮逐项核对输入组的 34 个候选、当前生产源码、可用接口约定及关联测试前置。结论均为 **STATIC**：本轮没有运行测试、没有做本地复现，也没有将旧报告或构建日志作为本轮验证证据；因此 JSON 中不使用 LOCAL_REPRO。测试修复建议只是可执行方案，仍需主代理统一定向运行。未修改 bugs、测试、生产代码、CMake 或 build 文件。

## 分类计数

| 决策 | 数量 | 含义 |
|---|---:|---|
| PRODUCT | 20 | 当前源码与正常生命周期/公开契约冲突，建议进入修复验收 |
| TEST | 2 | 产品主张由无效测试前置或期望支撑 |
| DUPLICATE | 4 | 同根问题并入已有 canonical |
| UNDECIDED | 7 | 输入契约、生命周期或重复调用语义仍缺证据 |
| NOT_BUG | 1 | 未初始化字段无读取，未形成可观察行为 |
| **合计** | **34** | |

## 逐项结论

| ID | 判决 | 结论与下一步 |
|---|---|---|
| BUG-R111-001 | PRODUCT | 策略工厂文档约定未知名称返回 NULL，CTA/UFT 仍解引用。修复 init 失败关闭，并补各 Mocker 未知策略名测试。 |
| BUG-R121-001 | PRODUCT | UFT 无 strategy 配置可 init 成功，但 session begin/end 无空守卫。扩展现有 no-strategy 用例覆盖四个会话入口。 |
| BUG-R121-002 | PRODUCT | 未知 executer 名依公开工厂契约返回 NULL，ExecMocker 仍解引用。当前测试只测 executer 节点缺失；另补未知名称失败测试。 |
| BUG-R191-001 | DUPLICATE → BUG-R64-002 | UFT 工厂释放 ABI 签名错配的同一问题，不重复计数。 |
| BUG-R201-002 | PRODUCT | 短仓 CSV 方向硬编码 LONG。现测试的 TM_LongT1 + 直接注入 short 无效，且只断言文件存在；改 TM_Both 并断言 SHORT 字段。 |
| R20_wtbtcore#1 | PRODUCT | EventNotifier 函数指针未初始化；当前 static 测试被静态零初始化掩盖。用自动存储期对象验证通知与析构。 |
| R20_wtbtcore#2 | DUPLICATE → BUG-L06-006 | 与 L06-006 同为 init(NULL)；Runner 正常路径先过滤空 cfg，空输入是否属于接口契约未定。 |
| R20_wtbtcore#3 | UNDECIDED | 缺 register 符号会裸调用，但插件 ABI 必需/可选符号表缺失；现测试需真实 MQ，未验证。用 fake module 确认合同。 |
| R20_wtbtcore#5 | UNDECIDED | pending cancel 重复调用会重复返还剩余量，但 API 未定义重复请求语义；当前测试还用了未 init fixture。先定拒绝或幂等合同。 |
| R20_wtbtcore#6 | DUPLICATE → BUG-N04-001 | ExecMocker init 失败仍回调与 N04-001 相同。 |
| R20_wtbtcore#8 | PRODUCT | MatchEngine 首 tick 创建 WTSTickCache，但无析构释放。建议 RAII/唯一所有权及多实例生命周期验收；泄漏规模本轮未测。 |
| R63#1 | TEST | R66 首 tick/时间格式、R67 复权标记和 cache 观察时点、R68 配置 JSON 构造存在测试输入/观察问题。另 ShmCaster 目录路径异常是独立 BUG-R68-002，不能混入 R63 测试裁决。 |
| BUG-R64-001 | PRODUCT | bool API 的两个空 map 分支返回空串指针，结果恒为 true。当前 disabled 用例调用 isHot 而不是有自定义 tag 的 isCustomHot，必须改测试目标。 |
| BUG-R64-002 | PRODUCT | UFT manager typedef 与插件 delete 导出按引用/按值不一致；R54 真实插件析构测试仍禁用，本轮静态判断。修复后重建插件并启用测试。 |
| BUG-R92-001 | PRODUCT | 默认撮合顺序可先擦除全成订单，再执行以 _orders[localid] 解引用的 entrust task。R72 用例提供正常订单流程夹具线索，但本轮未运行、未复用旧 crash 作为证据。 |
| BUG-L06-003 | PRODUCT | MatchEngine 批量撤单忽略 code，且没有跨订单累计 qty；现测试只含单合约、单笔。按接口注释扩为双 code/多笔。 |
| BUG-L06-004 | PRODUCT | total volume 非零但 askqty/bidqty 为零时被强制成交 1 手；当前用例只覆盖 total volume 为零。加对手盘口零量镜像用例。 |
| BUG-L06-005 | PRODUCT | MatchEngine 将任意品种 tick 用于所有订单撮合。加双品种逐 tick 隔离测试，回报 code 必须仍对应订单。 |
| BUG-L06-006 | UNDECIDED | EventNotifier init(NULL) 无守卫，但正常 Runner 路径拒绝 NULL；接口空值契约未找到。与 R20#2 合并裁定。 |
| BUG-L06-007 | DUPLICATE → BUG-P03-001 | 按额费率模板与缺 commodity 时在 calc_fee 同一行解引用，按量分支不使用该指针。配置是否受支持仍需 canonical 决定。 |
| BUG-N04-001 | PRODUCT | ExecMocker 失败前已登记 matcher，Runner 又忽略 init 结果注册 sink，handle_init 会解引用执行单元。失败路径应完全不进入回放。 |
| BUG-N04-002 | PRODUCT | reader 模块/入口失败后 _reader 仍空，却无条件 init 并返回成功。拆分缺模块、缺符号等失败测试，明确 false 与析构安全。 |
| BUG-P03-002 | UNDECIDED | 有持仓时动态盈亏缺 commodity 判空，但未证明回放中移除基础商品或加载未知代码是受支持生命周期。需确认快照/目录契约再定修复。 |
| BUG-P03-003 | NOT_BUG | 未初始化的 PosItem::_long 当前无读取；外层 PosInfo::_long/_short 才是方向容器。无可观察缺陷证据。 |
| BUG-P08-002 | TEST | 复现以 TM_LongT1（canShort=false）直接 on_trade 注入 short，绕过模式约束；TM_Both 是否有 short T+1 freeze 也无证据。该测试不能支持当前产品主张。 |
| BUG-P08-003 | PRODUCT | offset 1/2 的 vector erase 使用半开区间，最后一个被平为零的 detail 被排除；现有有效平今用例断言 details 为空。 |
| BUG-P08-004 | PRODUCT | stra_cancel_all 投递撤单却返回空 OrderIDs，与接口及示例调用方使用方式不符。缺直接测试，建议加多笔同 code 返回集合断言。 |
| BUG-P08-005 | PRODUCT | 平多应是 sell、平空应是 buy，但实现按持仓腿方向决定 undone 和盘口侧。现有测试以前置开仓再平仓验证，建议补平空镜像。 |
| BUG-P08-006 | UNDECIDED | unknown-code exit/cancel metadata 缺失空解引用，但支持契约未知；现有禁用用例先触发无 strategy 崩溃，且混入 NULL tick 等其他前置，不能定位。须拆项并确认目录生命周期。 |
| BUG-P08-007 | PRODUCT | IHftStraCtx 明确 qty=0 撤所有；实现只处理首笔。现有两买一卖测试已按正确期望断言，可定向运行。 |
| BUG-Q02-001 | PRODUCT | 空回放 replay_done 仍序列化未初始化日期/条件分钟；当前空 dump 测试未断言这两个字段。初始化并固定空快照值。 |
| BUG-Q02-003 | PRODUCT | 增量加载公开用途是继续回测，但 pending signal 丢失 desprice/sigtype，而后续 proc_tick 依赖两者影响执行与回调；现 roundtrip 只查 JSON/持仓。冻结持仓的快照边界另需明确。 |
| BUG-Q02-004 | UNDECIDED | loader 未验证解析错误、schema 和 tag 长度，但未找到外部损坏 JSON 属受支持输入合同的依据。明确跨版本/损坏策略后再分配产品修复。 |
| BUG-Q02-006 | UNDECIDED | loader 会 append，但 API 未承诺同一实例重复 load 幂等；现有测试每实例只 load 一次。先规定 one-shot 或可重复语义。 |

## 优先处理与待定项

优先进入产品修复的确定问题：R111/R121 工厂空返回处理、EventNotifier 指针初始化、MatchEngine 多品种隔离/零盘口量/批量撤单、N04 失败关闭、R92 订单任务生命周期、UFT 平仓方向/明细清理及 HFT qty=0 全撤、CTA 快照确定性与待处理信号恢复。

可直接修正的测试缺口包括：R201 短仓 CSV 用例（TM_Both + SHORT 断言）、R64 未知品种调用 isCustomHot、R20 EventNotifier 将 static 改自动对象、R121 UFT no-strategy 加 session 回调、R121 Exec 增未知执行器名、P08 disabled 空值家族拆为隔离前置、R63 的 tick/时间/复权码/config 输入、R20 重复撤单用成功 init fixture。按此顺序/具体过滤器见 triage JSON 的 proposed_test_fix 字段；所有这些过滤器均是建议的新/调整用例名，本轮没有执行。

UNDECIDED 的合同缺口应先由主代理确定：EventNotifier 空 cfg 与 MQ 符号必需性、重复撤单语义、短仓跨日冻结语义、运行中缺 commodity/unknown-code 生命周期、增量快照损坏输入及重复装载语义。JSON 为这些条目给出最小的分流验收要求。
