# L06 WtBtCore 缺陷记录（迭代 01）

本次只读复核生产代码、测试输入与跨报告重复项；未运行测试，以下“成立”均为静态代码证据，不表示已动态复现。原状态字段保留为「待审核」，并在每条中给出本次裁定。

## BUG-L06-001

- 状态: 已合并（BUG-R43-002）
- 本次裁定：代码中的买盘深度取错字段属实，但与 `bugs/R43.md` 的 BUG-R43-002 是同一行、同一根因；不得作为第二个独立产品缺陷计数。
- 位置：`src/WtBtCore/MatchEngine.cpp:230-247`，尤其 `:242-246`。
- 证据：以 bid 价作为 map key 时，赋给 `_items[px]` 的却是 `curTick->askqty(i)`；买盘账本记录卖盘数量。该账本的数据可被后续排队位置估计使用，因此不是单纯变量命名问题。
- 测试边界：`l06_match_engine_test.cpp:394-409` 的 `updateLobHandlesEmptyBookWithoutCrash` 只检查空盘口和双边盘口不崩溃，没有读取或断言买盘深度。用它不能证明该字段正确，也未动态复现本项。
- 修复方向：bid 价位存 `bidqty(i)`，ask 价位存 `askqty(i)`。用不相等的 bid/ask 数量构造 tick，分别断言账本两侧深度与输入一致。

## BUG-L06-002

- 状态: 已合并（BUG-R43-002）
- 本次裁定：单边买盘下对 `end()` 解引用的未定义行为成立；与 `bugs/R43.md` 的 BUG-R43-002 完全重复，不另计。
- 位置：`src/WtBtCore/MatchEngine.cpp:250-260`。
- 证据：只有 bid 档且 `ask_px == 0` 时，`sit = lower_bound(bid_px)` 命中唯一买档，随后 `sit++` 成为 `end()`；`:259` 仍读取 `sit->first`。这条路径不要求测试用例通过私有成员访问。
- 测试边界：L06 的空盘口用例没有单边 bid。R43 中对应的 `DISABLED_oneSidedBookDereferencesEnd` 仅是归档输入，本次未运行；因此这里只报告源码证明的 UB，不声称崩溃已动态复现。原“涨停封板必崩”也过强，标准只保证行为未定义，不保证每次崩溃。
- 修复方向：先处理 bid/ask 任一侧缺失的情况；对 `sit`、`eit` 在解引用前检查 `!= end()`，并仅在确认区间非空且有序时擦除。增加正常启用的单边 bid、单边 ask 与空盘口用例。

## BUG-L06-003

- 本次裁定：按合约和方向撤单的合约过滤、数量累计均有真实实现缺陷。
- 位置：`src/WtBtCore/MatchEngine.cpp:364-391`；接口注释见同文件 `:355-362`。
- 证据：`stdCode` 被注释掉且循环从不比较 `ordInfo._code`，所以指定合约会影响同方向的其他合约。`left` 在每次循环体内重新设为 `qty`，跨订单的 `left -= ordInfo._left` 不会累计；同时 `(int32_t)left` 会截断小数数量。结果可能是错撤其它合约、撤单总量超过目标，或目标量与订单量边界处理错误。
- 测试边界：`cancelBySideCollectsActiveOrdersOnly`（`:345-372`）只创建一个合约的订单，并传 `qty=0` 全撤；它不能证明合约过滤或正数量累计正确。本次未运行该测试。
- 修复方向：按 `_code` 和方向共同过滤；把剩余目标量放在循环外，使用 double/项目既有 decimal 比较累计整笔订单的 `_left`，仅在累计达到目标后停止。测试至少覆盖 A/B 两合约、同向多笔订单、`qty=0` 全撤、目标量小于/跨越首笔订单，以及小数数量是否受支持。

## BUG-L06-004

- 本次裁定：对手盘数量为零时凭空成交 1 手的分支属真实产品缺陷；double 精确相等可能造成幽灵挂单的说法目前证据不足，不作为已证缺陷。
- 位置：`src/WtBtCore/MatchEngine.cpp:96-154`（买单），`:157-211`（卖单）。
- 证据：tick 总成交量为 0 时会在 `:96` 早退；但主动买单使用 ask 一档数量（`:105-108`），主动卖单使用 bid 一档数量（`:163-166`）。若该对手盘数量为 0、tick 总成交量仍大于 0，`:141-143` 或 `:199-201` 把 `qty==0` 改成 1 并发送成交回报。此结论限于主动单；被动单使用 tick 总量推进排队，不能用“对手档量为零”直接证明同一行为。
- 测试边界：`zeroVolumeTickSkipsMatching`（`:287-303`）只设置 tick 总成交量为 0，会被 `:96` 早退；`positiveBuyMatchesAtAskPrice` 使用非零 ask 数量。两者都没有覆盖“总量非零、主动单对手盘数量为零”。小数 `_left` 是否会因精确比较而留下非零残量，需要小数量分批成交用例验证；满量分支以同一 double 值相减会得到精确零，不能仅凭 `== 0` 推断必留幽灵单。
- 修复方向：不要将零可成交量替换为 1；无对手量时不产生成交，剩余量按 `decimal::eq` 一类既有容差规则判断。补主动买/卖、总量非零但对应对手档量为零的双向用例；另用可表示的小数分批量独立验证尾量清理。

## BUG-L06-005

- 本次裁定：跨合约 tick 会激活并撮合其他合约订单的过滤缺失成立。
- 位置：`src/WtBtCore/MatchEngine.cpp:51-64,74-79,420-443`。
- 证据：`fire_orders(stdCode, ...)` 遍历所有 `_state == 0` 的订单，不检查 `ordInfo._code`，回报也使用当前 tick 的 `stdCode`。随后 `handle_tick()` 将同一 `curTick` 交给 `match_orders()`；后者同样遍历全部订单、未按合约过滤，并用这笔 tick 撮合。因此 B 合约 tick 能激活或按 B 的盘口撮合 A 合约的订单，回报代码也可能变为 B。
- 测试边界：L06 撮合用例每次只在一个合约上建单和喂 tick，没有 A/B 交叉输入。本次由循环与调用链静态证明，未运行复现。
- 修复方向：按订单代码过滤激活和撮合；entrust/order 回报使用 `ordInfo._code`。在 A、B 均有待激活买卖单时只喂 B tick，断言只有 B 的单发生状态变化；再喂 A tick 处理 A 单。

## BUG-L06-006
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）

- 本次裁定：直接对 `EventNotifier::init(nullptr)` 调用会解引用空指针，但目前没有证据表明该调用属于受支持输入；不列为已确认产品缺陷。可作为入口防御增强。
- 位置：`src/WtBtCore/EventNotifier.cpp:66-70`；正常调用路径 `src/WtBtPorter/WtBtRunner.cpp:626-632`。
- 证据：`EventNotifier::init()` 首次访问 `cfg->getBoolean()` 前没有空值检查。但 `WtBtRunner::initEvtNotifier()` 在调用前会拒绝 NULL 或非 object 配置，因此正常 runner 配置路径不把 NULL 传入。`inactiveConfigSkipsModuleLoading`（`l06_wthelper_event_test.cpp:97-107`）传的是有效 object 且 `active=false`，不是空指针复现。
- 测试边界：`init(nullptr)` 是当前文档自行设定的输入，没有找到接口注释承诺可接受 NULL 的证据；本次未运行。
- 修复方向：若 `EventNotifier::init` 对外承诺容忍缺配置，入口增加 `cfg == nullptr` 检查并补断言 `false` 的测试。否则保持 runner 侧校验，并把记录标为未确认的防御性改进，不纳入产品缺陷汇总。

## BUG-L06-007

- 本次裁定：按额计费模板匹配但 commodity 缺失时的空指针解引用成立；与 `bugs/p03_btcore_mockers2.md` 的 BUG-P03-001 完全重复，应只保留一个根因计数。
- 位置：`src/WtBtCore/HisDataReplayer.cpp:2946-2979,2983-3018`，特别是 `:2993-3008`。
- 证据：`loadFees()` 按费率文件 key 写 `_fee_map`，没有核验基础 commodity 是否存在。`calc_fee()` 在模板命中后获取 `commInfo`；仅按额分支于 `:3007` 调 `commInfo->getVolScale()`，无空值检查。故需同时满足 fee template 命中、`_by_volume == false`、commodity 缺失；按量分支并不使用该指针，不能把影响范围写成所有 fee template。
- 测试边界：`l06_replayer_runtime_test.cpp:143-147` 的未配置费率路径是 `_fee_map` 未命中并返回 0，不覆盖“费率模板存在但 commodity 不存在”。本次未运行，也未独立构造该配置。
- 修复方向：在按额计算前验证 `commInfo`；同时在加载费率模板时报告与 commodity 基础数据不匹配。若 `calc_fee()` 需要区分配置错误与合法零费用，应提供明确错误通道，避免静默返回看似有效的费用。
