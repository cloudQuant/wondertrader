# T26 CTPLoader 缺陷复核（2026-10-29）

本轮只核查该台账各项涉及的源码、现有测试和本机既有测试二进制；未修改生产代码或测试。`CoverageTests_t_CTPLoader` 的 3 条启用用例均通过：`t26_ctploader_flow.incremental_login_reloads_commodities_and_contracts`、`t26_ctploader_flow.last_instrument_response_with_fees_switch_queues_both_rate_requests`、`t26_ctploader_dump.margin_response_dumps_fees_to_fixed_relative_name_with_product_fallback`。队列用例的真实名字是单数 `switch_queues`；原先使用的 `switches_queues` 过滤名匹配不到用例（`NORUN`），已用正确名称重跑为 `PASS`。所有 `DISABLED_` 用例本轮未执行。测试通过只表示当前测试所断言的行为成立，不证明符合业务契约。

## BUG-T26-001 — worker 空队列时析构无法唤醒
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）

- 复核状态：`条件性生命周期缺陷；常规一次性 exit 路径未证为受影响`。
- 证据：`TraderSpi.cpp:239-271` 创建 worker；内层 `while (_queries.empty())`（`:243-246`）没有检查 `_stopped`。析构只设置 `_stopped=true` 并 join（`TraderSpi.h:24-29`），空队列时线程不会回到外层循环检查标记；`_stopped` 本身也是跨析构线程读写的普通 `bool`，存在同步缺口。
- 边界：`DISABLED_t26_ctploader_flow_hang.destructor_joins_forever_after_queue_drained` 本轮未运行。代码主流程使用 `exit(0)` 的说法不等于已验证所有进程/宿主生命周期；风险限于 worker 已启动后走普通析构的使用方式。
- 修复方向：将等待改为有谓词的条件变量/可中断等待，在设置停止标记时通知；同步保护队列状态。不要用单纯增加析构超时来掩盖未收尾线程。

## BUG-T26-002 — `fillCzceCode` 对不含数字的输入越界扫描

- 复核状态：`条件性输入健壮性缺陷；无效代码是否可由柜台/产品输入尚待证`。
- 证据：`TraderSpi.cpp:318-324` 的 `for(;;)` 递增 `pid_len` 并调用 `isdigit(code[pid_len++])`，不在 `\0` 处结束；输入没有数字时会越过字符串边界。
- 边界：`DISABLED_t26_ctploader_flow_crash.fill_czce_code_on_letters_only_scans_past_terminator` 本轮未运行。`"SRCS"` 是人工构造的非典型合约代码；本地 CTP 头文件未证明柜台可能发出此形状，因此不能外推为常规 CZCE 回报必现崩溃。
- 修复方向：循环检测 `\0`，若找不到数字则安全返回错误/原码；测试覆盖含数字合法代码及无数字、空串输入，不要求进程崩溃才算证据。

## BUG-T26-003 — 期权 JSON 写入嵌套、读取却访问顶层

- 复核状态：`确认的 dump/增量回读往返缺陷`。
- 证据：`DumpToJson()` 将期权字段写入 `option` 对象（`TraderSpi.cpp:801-814`）；`LoadFromJson()` 只检查该对象存在，却从合约顶层取 `optiontype/underlying/underlyingscale`（`:662-668`）。修正 `incremental_login_reloads_commodities_and_contracts` 的现状式断言后，单次定向运行 FAIL：输入与 dump 结构一致，类型、标的及 scale 实际为默认值，预期与输入一致。
- 修复方向：取得 `option` 子对象并在其上读取各字段；保留无 `option` 合约的默认行为。测试应检查嵌套期权四字段往返一致。

## BUG-T26-004 — strike price 读取了 product 键

- 复核状态：`确认的独立字段映射缺陷；与 T26-003 同一往返路径但不重复`。
- 证据：`TraderSpi.cpp:666` 把 `getDouble("product")` 赋给 `m_strikePrice`，而 dump 将执行价写为 `option.strikeprice`（`:807-813`）。`incremental_login_reloads_commodities_and_contracts` 的 `"product":"SR"`、`"strikeprice":5700` fixture 读回 strike 为 0。即使修复 T26-003 的子对象读取，这个错误键仍会使 strike 读错。
- 修复方向：在 option 子对象上读取 `strikeprice`，测试断言其值与写入值相同；不要把当前 0 当成产品默认契约。

## BUG-T26-005 — 增量回读丢失 altcode，费率查询字段还存在独立条件错误

- 复核状态：`确认的 JSON 字段丢失；下游错码结果依赖 CTP 接受的合约码`。
- 证据：dump 在 `TraderSpi.cpp:784-789` 写入 `altcode`，contract 装载 `:651-676` 没有读取它。`incremental_login_reloads_commodities_and_contracts` 输入 `altcode=SR510`，读回为空。之后 commission 查询 `:197-204` 和 margin 查询 `:217-225` 选择 code/altcode；若空 altcode 导致发出展开后的 `SR2510`，是否被柜台拒绝需以实际接口/回报验证。
- 额外独立问题：commission 查询的 `if(cInfo.m_strAltCode.empty())`（`:201`）包住了三元选码的 `strcpy`；因此 altcode **非空时反而不会填写 `InstrumentID`**。只补 JSON 读取并不能修复 commission 请求。margin 分支无此外围条件。
- 修复方向：装载 `altcode`；移除 commission 处错误的外围条件并统一选码；分别断言有 altcode/无 altcode 时 commission 和 margin 请求的 `InstrumentID`。不要在没有柜台响应证据时断言“所有 CZCE 费率查询必然空回报”。

## BUG-T26-006 — 相对 feesfile 配置被忽略

- 复核状态：`确认的配置行为不一致`。
- 证据：配置入口将 `feesfile` 读入 `FEES_FILE`（`CTPLoader.cpp:104,146,176`）；`DumpFees()` 仅在绝对路径分支使用该值，普通相对路径却固定写 `SAVEPATH/fees.json`（`TraderSpi.cpp:711-721`）。修正后的 `margin_response_dumps_fees_to_configured_relative_name_with_product_fallback` 定向运行 FAIL：测试配置 `custom_fees.json`，实际生成 `fees.json`，配置名文件不存在。
- 修复方向：将相对 `FEES_FILE` 拼到 `SAVEPATH`，保留文件名；测试同时覆盖相对、绝对配置，并避免只检查默认名。

## BUG-T26-007 — DumpFees 在费率键缺失时解引用 end iterator

- 复核状态：`静态条件风险成立；测试夹具不代表正常查询流，异步收尾前提也未验证`。
- 证据：`DumpFees()` 从 `_fees` 按 raw code 查询，再按品种查询；两次均 miss 后仍解引用 `it->second`（`TraderSpi.cpp:694-700`）。`_contracts[rawCode]` 还会在 key 不存在时插入默认合约。`DISABLED_t26_ctploader_dump_crash.fees_dump_of_unknown_margin_code_dereferences_end_iterator` 通过直接塞入 `ZZ99` 保证两张表都缺项；本轮未运行。测试源码 `t26_ctploader_dump_test.cpp:320-323` 的注释把它误标为 BUG-T26-003，应改成 T26-007。
- 证据边界：测试人为制造了 `_margins` 中存在、但没有正常查询来源的 `ZZ99`，不能证明该输入由正常 CTP 流程产生；也没有本轮柜台证据说明真实费率缺项的回报形状。发送队列清空与异步响应收齐的区别单列为 BUG-T26-011，避免把两种根因混为一项。
- 修复方向：查找不到 fee 时明确跳过或记录错误，不解引用 `end()`；用已登记合约、缺少 instrument fee 的有效响应夹具测试；以请求/响应计数或每请求终态决定何时落盘，并验证回报乱序时不会提前退出。

## BUG-T26-008 — `_trading_day` 构造时未初始化

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）

- 复核状态：`非标准调用顺序下的条件风险`。
- 证据：`TraderSpi.h:76` 声明 `_trading_day` 未设初值，登录回调在 `TraderSpi.cpp:171` 写入；CZCE 合约代码和期权 underlying 转换分别在 `:423`、`:442` 使用它。
- 边界：正常登录成功后才调用 `ReqQryInstrument()`（`:173-176`），因此正常链路先写后读；本轮没有证据显示柜台在登录前发合约回报。不能把普通运行的随机年份输出作为已复现故障。
- 修复方向：初始化为 0，并在日期未就绪时拒绝/延迟 CZCE 日期换算；对未登录回调测试安全失败。


- 迭代02复核: NOT_BUG。_trading_day 确实没有构造初值，但当前生产链先在成功登录回调写入交易日，再调用 ReqQryInstrument；合约回调通过该请求到达，CZCE 日期换算因此在正常链路中先写后读。未发现仓库生产路径在该赋值前读取它。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
## BUG-T26-009 — LoadFromJson 重复 load 且外层 root 未释放

- 复核状态：`源码可确认的短生命周期内存泄漏；运行时泄漏未测`。
- 证据：合约文件在 `TraderSpi.cpp:639` load 到外层 `root`，随后 `:642` 对相同路径再次 load 并以同名局部变量遮蔽；循环使用并在 `:679` release 的是内层对象。外层对象丢失引用，无法 release。
- 影响边界：每次增量装载多解析一次文件并泄漏一棵配置树；loader 的复用次数/运行时间决定实际影响，本轮没有 Instruments/Leaks 计量。
- 修复方向：使用 `:639` 得到的 `root` 遍历，移除重复 load，确保每个返回对象恰好 release 一次。

## BUG-T26-010 — 查询队列读写同步不一致

- 复核状态：`确认的线程安全缺陷；本轮未用 TSan 或压力测试复现`。
- 证据：生产者 `AppendQuery()` 在 push 时加 `_mtx`（`TraderSpi.cpp:230-235`）；worker 对 `empty()/front()/size()` 无锁读取（`:243,258,269`），只在 pop 时加锁（`:261-265`）。`std::queue` 不是并发容器，同一对象读写缺少共同同步会构成数据竞争。`last_instrument_response_with_fees_switch_queues_both_rate_requests` 本轮 `PASS`，只验证请求最终被 stub 观察到，不验证并发无竞态。
- 修复方向：队列的所有读写都在同一锁下，或改用条件变量队列；锁内只取出 task，执行 API callback/request 时释放锁，避免扩大临界区。补 TSan/并发压力验证。移除原稿中“ARM 弱内存一定读到中间态”和 ostringstream 堆破坏的因果表述：没有证据将这些现象归因于该队列。

## BUG-T26-011 — 发送队列清空被当作费率响应全部结束

- 复核状态：`异步收尾候选；源码判据明确，厂商响应时序与动态结果待验证`。
- 证据：`OnRspQryInstrument(bIsLast=true)` 将 commission 和 margin 查询任务排入 `_queries`（`TraderSpi.cpp:527-540`）。worker 消费并 pop 后 `_queries` 可为空，但这只说明请求发送任务已耗尽。两个异步回调 `OnRspQryInstrumentCommissionRate`、`OnRspQryInstrumentMarginRate` 随后只要看到 `_queries.empty() && QRYFEES` 就各自执行 `DumpToJson(); DumpFees(); exit(0)`（`:550-575,578-592`）。因此某个费率回调到达时发送队列虽空、其他请求仍可能处于 in-flight；若该时序成立，进程会在收到其余回报前落盘并退出。
- 边界：本轮未用 fake API 重排两个响应，也未连真实 CTP 验证响应是否可能晚于最后一个请求的发送。已有 `last_instrument_response_with_fees_switch_queues_both_rate_requests` 只验证两条请求被发送，不能验证回调收齐后才退出。此候选与 T26-010 的 `std::queue` 数据竞争是不同问题：即使队列锁正确，队列空仍不代表异步响应完成。
- 修复/验证方向：分别跟踪待发送任务与 in-flight 请求/每个 instrument 的 commission、margin 终态；仅在所有请求都有成功、空结果或错误终态后落盘。先依据厂商 API 合同确认成功/空结果/错误回调是否都有 `bIsLast`，再用 stub 按“最后请求发送前、最后请求发送后、两类响应乱序、响应缺失”控制回调，证明不会提前 `exit(0)`。

## 汇总

| 处置 | 编号 | 结论 |
| --- | --- | --- |
| 源码确认的功能/并发问题 | T26-003、004、005、006、010 | T26-005 的柜台拒绝后果、T26-010 的运行时触发率仍需验证 |
| 异步流程候选 | T26-011 | 队列为空的代码条件已确认；提前退出可达性需厂商时序和 mock 重排验证 |
| 条件风险/需验证正常前提 | T26-001、002、007、008、009 | 不把人工坏输入或未运行的禁用测试表述为已复现生产故障 |
| 测试注释已修正 | T26-007 | dump 禁用测试注释已改为正确编号；缺项危险路径仍 NOT_RUN |
| 动态检查 | 本轮 3 条启用用例均 PASS | 使用本机既有二进制；没有冷构建、没有运行禁用项、没有真实柜台验证 |
