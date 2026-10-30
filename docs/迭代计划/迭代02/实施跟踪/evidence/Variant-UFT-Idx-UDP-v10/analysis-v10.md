# UFT、指数、Variant 与 ParserUDP 本机验证（v10）

日期：2026-10-30。以下只记录 macOS arm64 focused 构建和测试结果。主代理和独立复审未发现本批修复的逻辑 blocker；`TEST_PASS` 表示该本机定向范围通过，不代表跨平台或完整产品验收。

## BUG-R11-009：UFT 首笔 tick 丢失

- 根因：工作线程已启动而分钟位置尚未建立时，首笔行情只初始化分钟位置，未走向引擎/策略的行情转发路径。
- 修复：首次有效 tick 在建立时间/分钟基线的同时转发到 `WtUftEngine`；后续同分钟 tick、分钟推进和自动闭合仍使用正常处理顺序。
- 验收：UFT focused suite 7/7 PASS。`firstSameDayQuoteBeforeStartupClockSetsTimeBeforeStrategy` 验证首次行情进入策略并更新日期/时间；`auto_close_after_check_time` 验证首 tick 进入引擎行情缓存、价格更新以及下一分钟闭合/行情推进。一个既有 `DISABLED_get_kline_slice_never_reaches_subscription` 被禁用，不计入通过数。
- 边界：本批是 fake session/engine 的 macOS 定向测试；真实 UFT provider、Runner 全链路、真实交易日夜盘、Windows、ASan/TSan 均未验收。本结论仅针对 UFT ticker，不能外推到 CTA/HFT。

## BUG-R54-002：parser 回退查到合约资料但 tick 未携带 `ContractInfo`

- 根因：`ParserAdapter::handleQuote` 可由基础数据管理器查到合约，但此前没有把借用的合约指针附回 quote；有持仓时，后续 context 缺少合约/商品元数据，无法正确计算浮盈。
- 修复：fallback 成功时将基础数据管理器所有的 `ContractInfo` 作为借用指针写入 quote，再规范合约代码；context 对缺失合约/商品资料采取防御处理，并仍转发行情。
- 验收：同一 UFT focused suite 内集成 `ParserAdapter -> WtUftTicker -> WtUftEngine -> UftStraContext` 的 1 项回归通过；用有仓 fixture 验证浮盈、quote 元数据、策略回调和引用计数，并用缺失元数据负控验证行情仍到达策略。
- 边界：Parser API 为本地 fake；供应商数据、真实 Runner/插件生命周期、跨平台和 sanitizer 未验收。

## BUG-N03-001：非法 `weight_alg` 越界

- 根因：配置入口曾接受任意 `uint32` 算法值，之后将其用于仅有 0、1、2 三项的算法表，非法配置可越界。
- 修复：在合约查询、订阅等副作用之前解析并校验权重算法，只接受 0/1/2；其他值受控拒绝并记录错误。保留受支持整数、精确浮点值和整数/科学计数法字符串的兼容输入。
- 验收：IndexFactory focused suite 13/13 PASS。覆盖算法 0/1/2 正常初始化、非法 3 和 `UINT32_MAX` 拒绝且无订阅/推送副作用，以及受支持的实数和字符串配置。
- 边界：macOS arm64 focused link；无 ASan/TSan、Windows 或完整 factory/生产配置联动证据。

## BUG-I02-NEW-013：WTSVariant 固定十位小数造成实数精度丢失

- 根因：`WTSVariant::asString()` 对实数使用固定 `%.10f`，较小差异值会映射成相同文本，例如 `1.00000000001` 可被写成 `1.0000000000`，再解析时不再是原始 double。
- 修复：改为以 C++ classic locale 输出，并从低有效位精度逐步增加到 `max_digits10`，选择能按 C numeric locale 解析回原 double 的最短表示；处理负零、无穷和 NaN。实数转整数访问器同时增加范围饱和，避免越界浮点转换的未定义行为。
- 验收：新 GTest focused 套件 8/8 PASS，包含 near-one、微小值、普通值、subnormal、最大值、特殊值、C++/C locale 与积分访问器；旧版项目内置 GTest 单独链接的兼容回归 1/1 PASS。两个 GTest 版本分开链接。
- 可观察变化与边界：实数字符串从固定十位小数改成最短往返文本；实数转整数越界由原有未定义/平台相关行为变为饱和。Windows、GCC 8、VS2017、ASan/TSan 未运行；locale 创建/释放的分配开销未测，旧消费者对文本格式的假设尚未做全量审计。

## BUG-I02-NEW-014：ParserUDP 零字节 datagram 后未重新挂起接收

- 根因：成功完成但 `bytes_transferred == 0` 时，receive handler 直接返回；当前 channel 没有下一项 receive，之后的合法行情数据报无法再被读取。
- 修复：零字节 UDP 数据报不交给 SPI、不当作成功行情，并在 channel 仍健康且连接 epoch 有效时重新 `arm_receive`；既有关闭和错误路径保持原语义。该缺陷属于 `ParserUDP`，与 `BUG-R103-001` 的 `TraderMocker::handle_read` 无关。
- 验收：ParserUDP focused suite 25/25 PASS。`EmptyBroadcastDatagramRearmsBeforeTheNextValidTick` 使用真实本机 UDP socket 先发空数据报、再发合法 TickPacket，并通过 `io_context::poll_one` 驱动回调，验证空包不产生 quote/close 且合法包随后到达。
- 边界：只覆盖本机 loopback 和当前 receive lifecycle；worker 并发竞态、V1 provider drain、用户回调持锁跨线程互调和真实网络持续故障未验收。Windows、ASan/TSan 未运行。

## 运行与链接边界

- 构建记录是增量对象构建，不是 clean rebuild 或 aggregate `CoverageTests`；focused link 使用现有静态库。
- 四个新版 focused binary 使用 `/usr/bin/c++ -arch arm64 -fprofile-instr-generate -g -Wl,-undefined,dynamic_lookup`；macOS 链接允许部分动态未解析符号，不可视为完整生产目标链接证明。
- Variant 旧版兼容测试使用仓库自带的旧 GTest 对象和单独生成的 main；不得把这些对象与 Homebrew 的 `libgtest`/`libgtest_main` 混链。
- 各日志、脚本、binary 和关键源码 SHA-256 见 `run-v10.json`。`reproduce-v10.sh` 保存对象构建、focused link、filter 和运行顺序。
