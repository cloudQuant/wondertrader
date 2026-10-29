# R9 候选项复核（迭代01 · WtBtPorter）

> 本轮只更新本记录，不修改生产代码或测试。依据是生产源码与现有测试前提的静态审阅；本轮没有运行测试。
> “源码证实”不等同于已运行复现；若触发路径依赖有效配置、行情存储或线程，均明确标为待验证。

- 关联用例: src/TestUnits/coverage/q06_btporter/r9_exp_mocker_bridge_test.cpp、src/TestUnits/coverage/q06_btporter/r9_mocker_wired_capi_test.cpp
- 关联既有记录: _hft_mocker/_exec_mocker 的构造遗漏见 Q06-003、R46-001；config 两个未生效守卫归入本文件 BUG-R9-001，不分别重复计数。

## BUG-R9-001 config_backtest 与 WtBtRunner::config 的幂等守卫均未置位

- 状态: 待审核
- 严重级别: Major（重复成功配置的支持契约与资源影响仍需核实）
- 位置: src/WtBtPorter/WtBtPorter.cpp:174-185；src/WtBtPorter/WtBtRunner.cpp:449-507
- 源码证据: C 导出 config_backtest 的局部 static bool inited 初值为 false，函数体从未写 true；WtBtRunner::_inited 只在构造列表中设为 false，在 config() 中读取但无写入点。配置成功路径会覆盖 _cfg，并在 CTA/HFT/SEL/Exec 分支分别 new mocker 后直接写入槽位；这些分支没有像 initCtaMocker 那样先删除旧 mocker。WtBtRunner 析构函数为空，源码也未见旧 _cfg 在重配前释放。
- 影响与证据界限: WtBtPorter.cpp 中 config_backtest 的实现注释声明配置重复调用会忽略，因此“守卫无效”违背现有注释契约。若重复成功配置属于受支持调用，第二次会再次进入 WtBtRunner::config，存在旧 _cfg/mocker 失去所有权以及重复初始化 replayer 的风险；未检查到成功配置后的内存增长或 replay 状态损坏运行证据，不能把泄漏量或后续故障写成实测结果。
- 测试契约: r9_exp_mocker_bridge_test.cpp::ConfigBacktestMissingFileLeavesMockersNull 两次使用不存在的配置文件。该用例只观察 mocker 槽仍为空；两次都保持为空无法区分 wrapper 是否跳过第二次调用，因此不能证明重复进入或成功路径泄漏。成功配置用例受有效配置/存储前置限制，当前是禁用占位；本轮未运行。
- 修复方向: 将一次性状态统一放在 runner 配置结果处管理，并只在成功配置后置位；配置接口应返回成功/失败，避免 C wrapper 在加载失败后无法决定是否可重试。若产品不支持重配，成功后明确拒绝而非覆盖；若支持重配，先定义并实现 _cfg、mocker、replayer sink 的完整释放/替换流程。

## BUG-R9-002 _inited 死守卫（并入 BUG-R9-001，不单独计数）

- 状态: 已合并（BUG-R9-001）
- 证据: WtBtRunner::_inited 在构造时写 false，config() 读取它，源码没有置 true 的路径。这是 BUG-R9-001 的 runner 内层守卫，与 C 导出层 static inited 共同导致相同的重复配置路径，不是另一项独立产品影响。
- 测试边界: 当前测试未能让 _inited 变为 true，也未验证该早退；不可达性是源码静态结论，不是覆盖率运行结论。
- 修复方向: 按 BUG-R9-001 统一配置状态与成功/失败语义，勿另算一项或只删除内层守卫而保留外层无效标志。

## BUG-R9-003 回测 HFT 包装把小时/半小时 K 线报成分钟周期

- 状态: 待审核（生产派发路径静态证据；尚无端到端运行复现）
- 严重级别: Major（若策略按回调 period 区分周期）
- 位置: src/WtBtPorter/ExpHftMocker.cpp:14-27；src/WtBtCore/HisDataReplayer.cpp:2030-2070；对照 src/WtPorter/ExpHftContext.cpp:29-43
- 源码证据: HFT 回测包装仅在 period[0]=='d' 时返回 period + times，其余周期都构造为 m{times}。真实回放派发在 HisDataReplayer::onMinuteEnd 使用基周期 "h"（小时）、"f"（半小时）、"m"（分钟）或 "d"（日线），而不是 "h1"/"m1" 这类完整字符串。接入 ExpHftMocker 后，("h",1) 与 ("f",1) 都会外发 "m1"；实盘 ExpHftContext::on_bar 采用 period + times，对应 "h1"/"f1"。分钟和日线的真实 base-period 输入可正确拼为 mN/dN。
- 测试契约与边界: HftBarPeriodRenamedByTimes 直接调用 on_bar，传入 ("m1",5)、("d1",3)、("w1",2)。这些不是 HisDataReplayer 的实际 (period,times) 参数形式；它们只证明包装对人工输入的格式化结果，"d13"/"w1"→"m2" 不能作为生产回放影响证据。测试没有覆盖实际的 ("h",1)/("f",1) 路径；本轮未运行。
- 修复方向: 若 Porter 回调要求完整周期串，按实际 base-period 统一格式化 period + times（或对 h/f 作显式映射），并用真实生产参数形状验证 m/d/h/f。测试记录需将 characterization 与生产回放结论分开。

## BUG-R9-004 CTA/SEL on_bar_close(NULL) 转发候选缺少可达前置

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: 不定；当前不按已证实产品缺陷计数
- 位置: src/WtBtPorter/ExpCtaMocker.cpp:133-139；src/WtBtPorter/ExpSelMocker.cpp:122-127；对照 src/WtBtCore/CtaMocker.cpp:945-960、src/WtBtCore/SelMocker.cpp:382-398
- 源码证据: 包装层 on_bar_close 确实会把收到的 newBar 转给外部 callback。但正常 CTA/SEL 路径先经过父类 on_bar，两个父类都在 newBar == NULL 时返回，因而不调用 on_bar_close。HisDataReplayer 的 bar replay 分支构造 WTSBarStruct 后再派发；本轮未找到正常回放会传 NULL 的生产调用点。period 空指针/空串的 period[0] 读取说法属于 ExpHftMocker::on_bar，不适用于本条 CTA/SEL on_bar_close。
- 测试契约与边界: CtaBarClosePassesPeriodUnchanged 直接调用内部 override 并传 NULL，证明该方法在人工注入 NULL 时会转发；它没有通过 CtaMocker::on_bar/回放器，也没有证明生产允许 NULL。HFT 的 NULL guard 对照不能建立 CTA/SEL 输入契约。本轮未运行这些测试。
- 修复方向: 先确认 on_bar_close 的内部契约是否允许 NULL。若允许，包装层在外发前判空并补正常派发路径测试；若不允许，应把测试限制在测试其真实契约的 bar 上，保留防御性空值处理作为单独设计选择。


- 迭代02复核: TEST。候选测试直接调用 on_bar_close(..., NULL)，绕过父类 on_bar 的 null guard；正常 dispatcher 只在有效新 bar 上进入 close 路径。当前证据只支持 wrapper 的直接调用行为，不支持产品可达路径会收到 NULL。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 证据不足（现有测试绕过正常派发前置）
## BUG-R9-005 微观结构回调忽略本地 _tick_subs 的订阅缺陷不成立

- 状态: 已撤回（原候选不是产品缺陷；现有测试的订阅前置与产品路由不匹配）
- 严重级别: 不适用
- 位置: 包装 src/WtBtPorter/ExpHftMocker.cpp:76-99；订阅/分发 src/WtBtCore/HftMocker.cpp:434-472,969-986；src/WtBtCore/HisDataReplayer.cpp:1795-1893,1925-2002,3113-3164
- 源码证据: _tick_subs 是 HFT tick 的本地过滤表；委托队列、委托明细、逐笔成交分别由 stra_sub_order_queues/details/transactions 注册到 HisDataReplayer 的 _ordque_sub_map/_orddtl_sub_map/_trans_sub_map，回放器只遍历这些订阅表后才调用 listener。ExpHftMocker::on_ordque_updated/on_orddtl_updated/on_trans_updated 是 listener 层的外部回调桥，因此这里不查 _tick_subs 不代表未订阅的代码会进入生产派发。覆写函数不调父类也不能单独证明错过状态更新；基类相应函数主要把事件转给内嵌 _strategy，外部 Porter wrapper 的职责是转给 C callback。
- 测试契约与边界: HftMicroStructIgnoresSubscription 只在 _tick_subs 放入 SSE.600000，随后直接手工调用 wrapper 虚函数并传 SHFE.rb2410；它绕过了三种 micro-data replay 订阅表，因此只能说明 wrapper 层不重复检查 tick 订阅。WtBtRunner::hft_on_* 对数据指针无 NULL 守卫，但正常 replay 先创建 WTSOrdQueData/WTSOrdDtlData/WTSTransData 再派发；没有证据证明 NULL 是该内部调用契约中的合法输入。本轮未运行。
- 处理方向: 不应据该测试要求在 micro callbacks 中检查 _tick_subs。若要保留缺陷项，需先通过 HisDataReplayer 订阅表构造“未订阅代码不派发 / 已订阅代码派发”的路由测试；只有内部回调契约明确允许 NULL 时才单独登记空指针防护问题。

## BUG-R9-006 _hft_mocker/_exec_mocker 构造遗漏（重复项，不单独计数）

- 状态: 已合并（主记录 BUG-R46-001；WtBtRunner 中 _hft_mocker/_exec_mocker 构造遗漏已覆盖）
- 严重级别: 见 BUG-Q06-003 / BUG-R46-001
- 位置: src/WtBtPorter/WtBtRunner.cpp:47-88；成员 src/WtBtPorter/WtBtRunner.h:635-638
- 证据与重复关系: 两个裸指针未在构造列表赋初值，是源码可证事实。Q06-003 已登记 _hft_mocker，R46-001 已列 _hft_mocker/_exec_mocker 及相关读取点，因此本条不构成新发现。
- 测试边界: MockersAreNullAndAsyncFalseBeforeInit 访问函数静态 getRunner()，受静态零初始化影响；InitHftMockerWiresSlotAndIsRepeatable 只验证 init 后槽位有值。均不证明普通自动/堆对象的未初始化值；_exec_mocker 也未由这些测试覆盖。本轮未运行。
- 修复方向: 按 R46-001 对照所有构造成员与消费者统一初始化；保留本项作交叉引用，不重复计数。

## BUG-R9-007 异步启动与 stop() 之间存在启动竞态；同步停止语义未证实

- 状态: 待审核（静态并发风险；未实际运行回测）
- 严重级别: Major（若 stop_backtest 的异步停止契约包含立即 stop）
- 位置: src/WtBtPorter/WtBtRunner.cpp:509-583；C 导出 src/WtBtPorter/WtBtPorter.cpp:223-226
- 源码证据: 异步 run() 创建 worker 后，_running=true 只在 lambda 开始执行时写入；stop() 先读 _running，若为 false 但 _worker 已存在，会直接 join 并 return，不调用 _replayer.stop()。因此若 stop 抢在 worker 的首条赋值之前，stop 请求可能退化为等待完整回放。_running 是普通 bool，由 worker 写、调用方读，缺少同步也形成 data race 风险。同步 run(false) 确实不设置 _running，但公共说明称 stop 主要用于异步模式；当前证据不足以把“同步 run 必须可跨线程中断”列为缺陷。
- 测试契约与边界: DISABLED_RunConfigSuccessPathsNeedStorageAndThreads 主体只有 EXPECT_TRUE(true)，是说明性占位，不能证明竞态、停止失败或同步中断。完整路径需要有效配置、存储和 worker；本轮未运行，也未测出其发生概率。
- 修复方向: 对异步模式，在 worker 可被 stop 观察前同步发布“正在启动/运行”状态，并用 atomic 或互斥量保护状态；stop() 应在 worker 可 join 时先发出 replayer stop 请求，再 join，且对“已完成”场景安全处理。用可控 barrier 的短回放测试覆盖 run 后立即 stop 和 worker 已进入 run 后 stop。同步 run 的可中断能力应另按公开契约决定。
