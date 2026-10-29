# 11_runners 专项复核

本组按第二轮输入复核 9 条 `(lane,id)`，逐条回看实现、公开接口/调用顺序、fixture 前提和测试 oracle；没有把旧 triage 结论当成本轮执行证据。写入范围限于本组报告、triage 和 evidence。

## 结论统计

| Decision | 数量 | 条目 |
|---|---:|---|
| PRODUCT | 5 | BUG-R14-005、BUG-R28-001、BUG-R28-006、BUG-R6-003、BUG-R6-004 |
| NOT_BUG | 4 | BUG-R14-006、BUG-R14-008、BUG-R22-004、BUG-R95-001 |
| LOCAL_INCOMPLETE | 0 | — |

8 条为 `STATIC`，BUG-R28-001 为 `LOCAL_REPRO`。本组首轮 7 个 enabled 单例中 6 个通过；未经初始化的局部 `WtRtRunner` 用例以 `-11` 退出，不能据此判定公共 C API 未知 parser 路径会崩溃。R28 最初一组 stack/heap 探针后来发现测试二进制将 `EventNotifier` 绑定到布局不同的 `WtBtCore` 实现，该组结果不作为产品证据。隔离测试目标依赖并重建后，link/nm/dyld 审计确认该目标使用本地 `WtCore` 的 `EventNotifier` 构造、析构和 `init` 符号；同一 fresh binary 上 stack 子进程构造后写入 `C`，析构窗口收到 SIGSEGV(11)，没有 `D`，未超时；heap 对照写出 `CD` 并通过。stack 外层 GTest 返回失败是复现信号的证据，不是 PASS。首轮命令、filter、目标 SHA 和输出见 `evidence/11_runners/`；正确链接后的 R28 日志、XML、SHA 和符号/链接记录见 `evidence/01_share/l00-3targets/`。

## 逐项判定

### BUG-R14-005 — PRODUCT / STATIC

UFT 与 HFT 的 `run()` 都在 parser、trader、engine 调用周围使用空 `catch (...) {}`：`src/WtLatencyUFT/UftLatencyTool.cpp:315-326`、`src/WtLatencyHFT/HftLatencyTool.cpp:494-505`。外层 `test_uft()` / `test_hft()` 按正常生命周期执行 `init()` 后再 `run()`，入口最终仍打印提示并返回 0：`UftLatencyTool.cpp:28-34`、`HftLatencyTool.cpp:33-42`、两个 `main.cpp:25-32`。因此运行异常会静默被当作成功完成；交互式 `getchar()` 本身不是缺陷。

本轮 UFT/HFT zero-tick 用例均返回 0，但只能证明无异常的空样本路径跑完。空 catch 使 `EXPECT_NO_THROW` 无法证明 parser/engine 尾部完成；HFT marker 只确认 engine 路径，不是 parser 完成 oracle。已向主代理请求保留 smoke 并补可控异常状态/诊断和 parser 完成标记，详见 triage 的两个 `test_change_requests`。修复验收需分别注入可控异常，确保运行失败有可观察状态/诊断，且正常有限样本仍完成。

### BUG-R14-006 — NOT_BUG / STATIC

UFT/HFT 不接收配置路径参数，均从进程 cwd 读取 `config.yaml` 和 `logcfg.yaml`，HFT 还读取 `actpolicy.yaml`：`UftLatencyTool.cpp:198-208`、`HftLatencyTool.cpp:347-357,398-407`。本轮正向零样本用例在有效 cwd fixture 下通过。原空 cwd 用例只建立“配置文件缺失”的条件；仓库未找到要求从可执行文件目录解析配置的合同。当前按 cwd 作为相对配置根解释，不能由空 cwd 失败推出产品读取路径缺陷。若部署要求 exe-relative，应作为独立启动合同提出。

### BUG-R14-008 — NOT_BUG / STATIC

`WtUftRunner::init()` 调用 `WTSLogger::init(filename)` 时未注册自身 handler，主程序仅执行 init/config/run：`src/WtUftRunner/WtUftRunner.cpp:53-58`、`src/WtUftRunner/main.cpp:64-79`。仓内没有该 runner 的 `registerHandler(&runner)` 调用；日志回调虽为空，但没有证据显示正常 runner 会进入该回调，也未找到 UFT 通知消费者或对外转发合同。现有 no-op 测试只检验不抛出，不能证明或反驳日志转发承诺；本轮将其判为未接入实现残留，不请求只为该猜测改测试。

### BUG-R22-004 — NOT_BUG / STATIC

`WtDtRunner::initialize()` 在解析配置前设定进程级 logger 和 module directory，随后解析失败分支记录错误并返回，没有进入服务初始化：`src/WtDtPorter/WtDtRunner.cpp:88-103`。C API 单例提供模块路径，helper 文档说明其供后续模块使用：`src/WtDtPorter/WtDtPorter.cpp:48-65`、`src/WtDtCore/WtHelper.h:31-43`。这属于为配置错误保留诊断能力的引导状态；未发现要求失败回滚 logger/module path 的公共合同。将来若要支持不同 logger 配置的失败后重试，应另行明确 logger 重配置语义。

### BUG-R28-001 — PRODUCT / LOCAL_REPRO

`WtRunner` 默认构造和析构均为 public，生产 main 普通路径使用栈对象：`src/WtRunner/WtRunner.h:46-58`、`src/WtRunner/main.cpp:96-126`。主代理新增的测试只调用默认构造和自动析构，不调用 init/config/run：子进程在构造返回后写 `C`，在析构返回后才写 `D`；父进程以 `waitpid(WNOHANG)` 做 2 秒墙钟限制，并回收原始状态：`src/TestUnits/coverage/m_WtRunner/r28_runner_main_test.cpp:171-278,397-423`。最初的探针二进制把 `EventNotifier` 绑定到 `WtBtCore` 实现；因其类布局与 `WtCore::EventNotifier` 不同，最初的 stack 崩溃不能用于判定产品。

隔离链接并重建后的 `CoverageTests_m_WtRunner` 仅链接匹配产品的 `WtCore`、`WTSTools`、`WTSUtils` 等依赖，不包含会提供冲突实现的 `WtBtPorter/WtBtCore`：`src/TestUnits/coverage/CMakeLists.txt:439-456`。`nm` 记录显示本地二进制定义了 `T wtp::EventNotifier::{EventNotifier,~EventNotifier,init}`；链接记录包含 `WtCore`，dyld 记录没有将这些符号导入 `WtBtPorter`。正确绑定后的 fresh binary SHA 为 `d0f36d4df0cbdae9035484d3dfc6833b3d3c1e904cea63d866ef49f10582d03a`：stack 过滤在 `C` 后、`D` 前收到 SIGSEGV(11)，日志用时 24ms、未触发 2 秒 watchdog；heap 对照在同 SHA 下输出 `CD` 并通过。详细记录见 `evidence/01_share/l00-3targets/stack.log`、`stack.xml`、`heap.log`、`heap.xml`、`runs.json` 及 `CoverageTests_m_WtRunner-{nm,dyld,link}.txt`。原 disabled 候选 `r28_runner_main_test.cpp:1084-1089` 还调用了缺失配置的 `config()`，并非纯构造/析构 probe；本轮不以它为执行依据。

这足以判定当前本机正确依赖绑定下，公开默认构造→析构生命周期存在产品缺陷：构造完整返回后，同一对象的析构窗口会崩溃，而堆对照可完成。它**不能证明栈地址本身是根因，也不能定位哪个成员触发**。只读源码发现若干默认子对象析构路径中的未初始化指针候选，包括 `WtCtaEngine`、`WtSelEngine`、`EventNotifier`，但当前信号和 marker 不足以把 SIGSEGV 归因给其中任何一个字段：`src/WtCore/WtCtaEngine.cpp:34-46`、`src/WtCore/WtSelEngine.cpp:33-56`、`src/WtCore/EventNotifier.cpp:37-55`。

修复应先由符号化回溯或逐个隔离子对象析构定位真实读取点，再初始化析构路径会读取的未初始化 raw/function pointer，并保证未配置对象的 release/destructor 安全；不要通过改用 heap 掩盖生命周期问题。验收时，fresh `CoverageTests_m_WtRunner` 上 `r28_wtrunner_lifecycle.stack_construct_destroy_reports_cd_in_child` 与 `r28_wtrunner_lifecycle.heap_construct_destroy_reports_cd_in_child` 都应写出 `CD`、waitpid 正常退出 0、无 signal 且未超时。该验收只覆盖默认构造/析构，不代表完整 config/run、插件或交易环境。

### BUG-R28-006 — PRODUCT / STATIC

`WtBtRunner` 从配置读取 `env.mocker`，只为 `cta/hft/sel/exec/uft` 建立 listener，随后无条件执行 replay `prepare()`/`run()`：`src/WtBtRunner/WtBtRunner.cpp:91-100,121-154`。`getCString()` 对缺失 key 默认返回空字符串：`src/Includes/WTSVariant.hpp:346-353`。`HisDataReplayer` 构造时 `_listener` 为 null，`prepare()` 在 669 行直接调用 `_listener->handle_init()`：`src/WtBtCore/HisDataReplayer.cpp:145-148,666-675`。因此未知/空 mocker（含缺失字段）配置会走到空指针解引用；这属于输入边界安全问题，支持模式范围已经由分支列出，报错还是回退属于后续业务策略。

现有 `.blocked` fixture 依赖行情/日历及 cwd 工厂，而且 coverage 清单没有对应 runner target：`src/TestUnits/coverage/m_WtBtRunner/r28_btrunner_main_test.cpp.blocked:312-322`、`src/TestUnits/coverage/CMakeLists.txt:141-155`。没有将其当作已执行测试。本条已向主代理提出纯 mocker 解析 seam/专属 target 请求，验证五个支持值和 unknown/empty/missing 在 `prepare()` 前受控失败。

### BUG-R6-003 — PRODUCT / STATIC

公开 Porter 文档要求初始化并注册 parser callbacks；`create_ext_parser` 单独文档只要求 callbacks，没有注明成功 config 是前置：`src/WtPorter/WtPorter.h:140-161,226-233`。导出函数转发到 `WtRtRunner::createExtParser()`：`src/WtPorter/WtPorter.cpp:149-156`。`WtRtRunner` 构造未初始化 `_engine`，仅在 `config()` 成功初始化 engine 后赋值；`createExtParser()` 却直接读取 `_engine` 及 manager：`src/WtPorter/WtRtRunner.cpp:55-100,295-303,838-882`。因此按公开文档注册 callbacks 后、config 前调用创建入口可触发未就绪指针路径。

配置后有效创建的 `R82PorterCapiReal.ExtParserQuoteSuccess` 正控本轮通过；并未运行危险的 config 前 probe。修复需初始化 `_engine` 为空并在未就绪时诊断并返回 false，同时在创建接口文档明示成功 config 的时序要求。已请求使用公开静态 C API 建立 callbacks、config 前安全拒绝，并保留 config 后正控；不要用未经初始化的局部 runner 做产品结论。

### BUG-R6-004 — PRODUCT / STATIC

`parser_push_quote` 是公开 C ABI，导出层把 id/tick 直接转给 runner：`src/WtPorter/WtPorter.h:1286-1296`、`src/WtPorter/WtPorter.cpp:2265-2268`。未知/空 id 的 map lookup 没有空值守卫；若 id 命中已注册 adapter，runner 在 `WTSTickData::create(*curTick)` 解引用 tick：`src/WtPorter/WtRtRunner.cpp:1243-1255`、`src/WtCore/ParserAdapter.cpp:406-414`。现有 active NULL tick 调用传入未注册 id，所以会先走 unknown-parser 分支，未覆盖 registered-adapter 解引用；原 disabled 混合用例也同时调用其他 NULL API，不能隔离归因。

本轮已跑 registered parser+有效 tick 正控并通过；C API unknown id+有效 tick 通过；`CallbackRegistrationAcceptsNullAndReplaces` 中的 unknown id+NULL tick 调用也随该混合用例通过，但它不能证明注册 parser 的 NULL tick 安全。另一个 active `R6PorterExtParserQuote.unknown_parser_leaves_the_supplied_tick_untouched` 在本轮 fresh binary 返回 `-11`，检查后发现它直接使用未经生命周期初始化的局部 `WtRtRunner`；相同 unknown-id 条件的静态 C API 正控通过，所以不把该崩溃作为公共参数路径复现。

已向主代理申请分别补空 id、注册 id+空 tick 两个边界测试，并将误用局部 runner 的 unknown-id 测试改为静态 C API 或完整生命周期。边界修复需在字符串 key 查找和 tick 解引用前分别校验；合法注册 id+有效 tick 继续恰好投递一次。

### BUG-R95-001 — NOT_BUG / STATIC

`WtUftRunner::config()` 未调用 `initEvtNotifier()`；该 helper 只有定义，没有生产调用点：`src/WtUftRunner/WtUftRunner.cpp:109-120,340-349`。`WtUftEngine::init()` 只保存 notifier 指针，仓内未找到 UFT engine 的实际事件消费点：`src/WtUftCore/WtUftEngine.cpp:308-319`、`WtUftEngine.h:151-157`。本轮有效配置 fixture 不含 notifier 且通过，符合当前配置形态；没有配置样例、接口文档或消费者支持“通知已承诺但未发送”的主张。当前按未接入 helper 判 NOT_BUG；将来支持 notifier 时需先明确 schema、事件、失败策略和消费者，再做端到端验收。

## 定向运行记录

收到主代理刷新 CoverageTests 目标的通知后，本组首轮仅对读过且确认可独立运行的 enabled 单例执行隔离进程；每例使用独立空 cwd、LLVM profile 路径、XML 和 15 秒墙钟限制。本组没有修改源码/测试/构建文件。之后主代理在同一 runner 测试文件新增并构建 R28 专用子进程 probe；其单独证据列在表后。

| Target / filter | 结果 | 判定用途 |
|---|---|---|
| `CoverageTests_m_WtLatencyUFT` / `r95_ufttool_run.zero_ticks_after_successful_init_exercises_engine_and_parser_tail` | 1 test, PASS | 正常空样本路径；不能覆盖被吞异常 |
| `CoverageTests_m_WtLatencyHFT` / `r95_hft_run.zero_ticks_after_successful_init_exercises_engine_and_parser_tail` | 1 test, PASS | engine marker 有效；不能单独证明 parser 尾部 |
| `CoverageTests_m_WtUftRunner` / `r95_uft_config.string_basefiles_and_array_modules_return_true` | 1 test, PASS | 无 notifier 配置正控 |
| `CoverageTests_porter` / `R82PorterCapiReal.ExtParserQuoteSuccess` | 1 test, PASS | 完成 config/create 后注册 parser 的正常路径 |
| `CoverageTests_porter` / `R6PorterExtParserQuote.unknown_parser_leaves_the_supplied_tick_untouched` | return code `-11`，无 XML | 局部 runner 未初始化，属于测试前置错误；不作为产品 repro |
| `CoverageTests_porter` / `Q06PorterCapi.ParserPushQuoteIgnoresUnknownParserId` | 1 test, PASS | 未注册 id + 有效 tick |
| `CoverageTests_porter` / `Q06PorterCapi.CallbackRegistrationAcceptsNullAndReplaces` | 1 test, PASS | 混合 callback NULL 及未注册 id + NULL tick；不命中已注册 adapter |

主代理补充的 R28 生命周期探针：

| Target / filter | 结果 | 判定用途 |
|---|---|---|
| `CoverageTests_m_WtRunner` / `r28_wtrunner_lifecycle.stack_construct_destroy_reports_cd_in_child` | 修正链接后子进程 signal 11，markers=`C`；未超时，外层 GTest 失败 | 正确 `WtCore` 绑定下构造完成、析构窗口崩溃；`LOCAL_REPRO` |
| `CoverageTests_m_WtRunner` / `r28_wtrunner_lifecycle.heap_construct_destroy_reports_cd_in_child` | 同一 fresh SHA，1 test PASS；markers=`CD` | 同目标 heap 对照完成 |

首轮原始 stdout、XML、逐例 metadata、目标 SHA 与命令见 `evidence/11_runners/`；修正链接后的 R28 原始日志、XML、目标 SHA 与符号/链接记录见 `evidence/01_share/l00-3targets/`。旧 `evidence/r28/` 中错误绑定版本的结果已排除。没有启用旧的 disabled 崩溃候选，也没有运行 config 前未就绪创建、空 parser id、registered id+NULL tick 或未知 mocker replay。`ASan/TSan` 未执行。

## 未完成事项

- BUG-R28-001：正确链接下已复现默认构造→析构生命周期缺陷；仍需符号化回溯或隔离 member destructor，定位具体触发的读取，才能锁定代码修复点。不得将其归因于栈地址或任何特定成员。
- 其余 PRODUCT 项的边界回归均已形成精确 `test_change_requests`，需主代理审核、修复/补测试后再运行；本报告不把静态结论提升为运行期修复验收。
