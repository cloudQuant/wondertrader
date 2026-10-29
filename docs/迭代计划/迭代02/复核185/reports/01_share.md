# 01_share 第二轮复核

本组复核 8 条：`BUG-L00-005`、`BUG-L00-009`、`BUG-L01-006`、`BUG-L01-009`、`BUG-L01-012`、`BUG-L01-014`、`BUG-L01-018`、`BUG-L01-019`。本轮重新核对了输入边界、公开接口、生产调用、测试前置条件及可运行的定向用例；产品源码没有修改。

## 汇总

| 裁定 | 数量 | 条目 |
|---|---:|---|
| PRODUCT | 3 | BUG-L01-006、BUG-L01-014、BUG-L01-018 |
| NOT_BUG | 4 | BUG-L00-009、BUG-L01-009、BUG-L01-012、BUG-L01-019 |
| LOCAL_INCOMPLETE | 1 | BUG-L00-005 |

## 逐条结论

### BUG-L00-005 — LOCAL_INCOMPLETE

原条目范围是 `WtCore`、`WtUftCore`、`WtBtCore`、`WtDtCore` 四个核心中的同名 `wtp::` 类，以及历史静态扫描记录的 37 个跨目录重复类和多个最终链接目标。最新只读静态清单覆盖 39 个最终 CoverageTests 二进制，发现四核心归档之间有 50 个重复 `wtp` 类名和 849 个完全相同的外部符号。旧 37 类计数与新 50 类清单尚未逐项对照，不能将它们视为已核实的一组。完整清单和矩阵见 `evidence/01_share/l00-full-audit/`。该扫描未运行 GTest，也没有核查全部实际调用路径，因此整体仍未闭环，不能因为已修目标或静态重名就把整个条目判为 TEST 或关闭。

本轮已检查若干最终目标，并对确认的测试链接归属问题作了目标级隔离：

- **WtUftRunner / ActionPolicyMgr**：修复前 r95 配置用例虽 1/1 PASS，但 ActionPolicyMgr 来自 WtCore 的 `libWtExecMon.dylib`，UftCore 覆盖未命中。CMake 为该目标改用 WtUftCore 依赖集后重建；r95 用例 1/1 PASS，最终链接的核心归档仅有 `libWtUftCore.a`，nm 为可执行体本地定义，dyld 无该符号导入，llvm-cov 显示 UftCore `init` 命中 1 次、WtCore 输出为空。
- **WtLatencyUFT / ActionPolicyMgr**：进一步检查发现 `getActionRules` 曾从 WtCore 的 `libWtExecMon` 导入。按产品核心依赖隔离并重建后，安全 r95 配置用例 1/1 PASS；最终链接使用 `libWtUftCore.a`，nm 显示 `ActionPolicyMgr::getActionRules/init` 为本地定义。
- **WtRunner / EventNotifier**：目标隔离为产品对应的 `WtCore`，最终链接的核心归档只有 `libWtCore.a`；nm 显示 `EventNotifier` 与 `ActionPolicyMgr` 为本地 WtCore 定义。正确绑定下，栈构造/析构用例仍因子进程 SIGSEGV(11) 失败 1/1，堆构造/析构对照 1/1 PASS。这个运行观察不能归因于跨核心符号错绑，应作为独立生命周期问题核查。
- **CoverageTests_porter / EventNotifier**：修正后最终链接不含 `WtBtPorter`；dyld 所列 EventNotifier 方法由 `libWtExecMon`（WtCore）提供，符合 WtPorter 目标的核心家族。安全 unknown-parser C API 用例 1/1 PASS；该用例是 C API 对照，符号提供者另由 link/nm/dyld 证据确认。

扩展只读 provider 筛查还发现：`CoverageTests_m_WtLatencyHFT` 的 `ActionPolicyMgr` 和 `WtHftEngine` 分别来自 WtCore 依赖的 `libWtExecMon` 与 `libWtPorter`，符合该产品目标的 WtCore 依赖；`CoverageTests_btporter` 的 `EventNotifier` 来自 WtBtPorter，符合 WtBtCore 依赖。对 `CoverageTests_wtcore/uftcore/btcore/dtcore` 只筛查了 EventNotifier/ActionPolicyMgr 的动态导入，未看到不匹配导入；该静态筛查不能证明其它同名类或静态归档成员归属正确。细节见 `evidence/01_share/l00-final-target-provider-audit.md`。

新矩阵显示的 dyld import 统计包括 `<weak-def-coalesce>` 3341 项、`libWtExecMon` 62 项、`libWtBtPorter` 3 项和 `<flat-namespace>` 1 项。特别是 `CoverageTests_wtcore` 的 `TraderAdapter::isTradeEnabled(char const*) const` 为未定义符号，dyld provider 是 `<flat-namespace>`，来源尚不能唯一确定，列为精确的本地剩余核查项。最终 image 中的 `nm T/W/V` 定义只证明符号存在于可执行映像，不能独立定位到某个归档成员；弱符号合并和 flat namespace 也不能唯一归属到核心家族。矩阵没有执行测试或运行时调用，故 39 个目标的静态枚举不等于 39 个目标都已动态验收。

这些目标级结果及修复均只代表已列目标，不覆盖四个核心和多个最终目标的全部符号。前后链接、符号、运行与覆盖证据见 `evidence/01_share/bug-l00-005-mixed-link/`、`evidence/01_share/l00-after/`、`evidence/01_share/l00-3targets/`、`evidence/01_share/l00-porter/`、`evidence/01_share/l00-final-target-provider-audit.md` 和 `evidence/01_share/l00-full-audit/`。本轮验证为 macOS arm64；Windows/MSVC 配置尚未运行。整体关闭前仍需定位 flat-namespace import、核实弱合并和静态 archive 候选的实际来源，并按最终目标核查四核心其余同名类的动态调用与源文件覆盖映射；历史 37 类数与新静态清单 50 类也待对账。同名本身不作为产品缺陷证据，只有确认行为/公开契约偏差才可判生产问题。

### BUG-L00-009 — NOT_BUG

`ParserYD.cpp` 的成员初始化列表顺序与头文件声明顺序不同，但相关值是常量 `0`/`NULL`，目前只形成严格编译告警风险。该厂商工程的 Visual Studio 配置为 Level3，未发现 `/WX`；顶层 CMake 与 CoverageTests 均未纳入 ParserYD。候选使用的 `-Werror,-Wreorder-ctor` 不是当前项目门禁，未证明当前运行缺陷或构建阻断。若将来把该目标纳入统一 warnings-as-errors 门禁，再按声明顺序整理并用真实厂商配置编译。

### BUG-L01-006 — PRODUCT

`TimeUtils::getNextMinute` 只修正一次 1440 分钟日界。公开参数为 `int32_t mins`，接口没有单日偏移限制；因此 `getNextMinute(0, -1500)` 是签名允许的输入。本轮刷新后的 `CoverageTests` 中，普通分钟用例 1/1 PASS，而大负偏移用例 1/1 FAIL：实际返回 `4294967196`，期望 HHMM 为 `2300`。根因是余下的负分钟未取完整模，转换到无符号结果后变成巨大值。仓内已找到的生产调用只使用 `-1/+1`，故本结论确认公开 helper 缺陷，不外推为已观察到的生产触发。

修复建议：使用 `int64_t` 计算总分钟并做完整正模 `((total % 1440) + 1440) % 1440`，再转换 HHMM；验收覆盖普通午夜、跨多日正负偏移及 `INT32_MIN/MAX`，所有结果在 `0..2359`。原始运行细节和 XML 见 `evidence/01_share/recheck-runs-second-round.json` 与同目录 `runs/second_get_next_minute_*`。

### BUG-L01-009 — NOT_BUG

`Time32::fmt()` 确实返回共享静态缓冲，本轮同线程定向用例 1/1 PASS 并验证了缓冲复用与覆盖。但头文件没有承诺跨调用保留或并发安全，仓内也未找到生产 `.fmt()` 调用。当前测试只确认实现行为，不能单独证明违反 API 合同；测试注释现已明确这是单线程观察，不声称线程安全或竞态。执行记录见 `evidence/01_share/recheck-runs-second-round.json`。

### BUG-L01-012 — NOT_BUG

候选中的非法月份码只在绕过前置检查、直接调用 `extractStdChnFutOptCode` 时触及风险路径。当前仓内受支持生产入口是 `extractStdCode`，它先用 `isStdChnFutOptCode` 校验后再进入 helper；没有找到生产代码绕过该检查的调用。本轮合法 CFFEX 提取和 wrapper 期权路径定向用例均 1/1 PASS。结论只撤回当前生产可达性主张，不代表任意外部调用者传入畸形字符串都安全。若 API 后续承诺接受任意文本，再为 helper 定义安全失败合同。

### BUG-L01-014 — PRODUCT

`rawFutOptCodeToStdCode` 的 fallback 假设输入已具有期权码形状：无符号 `idx >= 0` 循环在下溢后不会终止，且 `idx - 3` 可继续下溢。外部合同配置可形成 `CC_FutOption` 合约，解析器、数据服务和交易适配路径会将原始合约代码传入该 helper；未发现这些入口先验证 fallback 所需长度及分隔形状。故这是外部配置可达的未校验输入风险。有效 CZCE/SHFE 样例已覆盖；本轮没有运行已知会触发 UB 的畸形输入。

修复应先校验非空、最小长度、`P/C` 位置及数字段，并定义明确失败值；相关调用点和配置加载路径应阻止失败代码继续发布。产品修复后再增加 `rawFutOptCodeToStdCode("123456", "CZCE")` 安全失败回归，并在可用 sanitizer 下验证；修复前不得启用该畸形输入测试。详见 JSON 中的精确测试修正请求及 `recheck-runs-second-round.json`。

### BUG-L01-018 — PRODUCT

`getWeekDay` 接受 `YYYYMMDD`，没有声明 1970 年下界；`WTSBaseDataMgr::isHoliday` 将日期直接转发。`1969-12-31` 是有效公历日期。函数未检查 `mktime` 返回值，也未检查 `localtime` 空指针便解引用。Microsoft CRT 文档明确了该有效日期在 Windows CRT 下会进入 `mktime` 失败/负时间处理路径；本机 macOS 对照探针成功，不作为 Windows 运行证据。普通日期定向用例 1/1 PASS，Windows/MSVC 尚未运行。

优先考虑平台无关的公历星期算法；若保留 CRT，应验证日期和返回值、定义可传播的失败结果并检查所有调用方。验收要求 `19691231 -> 3`、`20240101 -> 1`，非法日历值显式失败且无参数处理器终止或空指针解引用，并在 Windows/MSVC 与 macOS 验证。平台文档、macOS 对照探针及本轮用例分别见 `microsoft-time-range.md`、`macos-time-range-probe.json` 和 `recheck-runs-second-round.json`。

### BUG-L01-019 — NOT_BUG

候选依据是 `CodeHelper.hpp` 中过时的 `SSE.600000 -> SSE.STK` 示例。0.8.0 更新日志明确规定规范股票码为 `SSE.STK.600000` 且不再支持 `SSE.600000`；历史提交 `1f5f2b3c687a3393ecdf321932d22cc4b6dc3814` 删除了简写映射。当前两段码原样返回符合生效合同，仓内没有该 helper 的生产调用。产品行为不应恢复已废弃简写；当前测试注释已改为规范输入说明。

当前源码的 R12 精确过滤器 `r12_codehelper.std_code_to_std_comm_id_and_opt_raw` 已在刷新后的 `CoverageTests` 上 1/1 PASS；L01 对照过滤器 `l01_codehelper.std_code_to_std_comm_id` 也为 1/1 PASS，二者使用二进制 SHA-256 `99f535d45e79aa664cf767dfb86d541fcc419386a5f88caf9af8179fe2972556`。当前测试验证规范输入 `SSE.STK.600000 -> SSE.STK` 和两段 `SSE.STK` 恒等，不再要求已废弃简写。历史合同和本轮运行记录见 `evidence/01_share/sse-code-contract-history.md` 与 `evidence/final-fix/{runs.json,stock.log,stock.xml,code.xml}`。头文件仍保留历史示例，但不改变 0.8.0 生效合同或当前测试结果。

## 证据边界与后续

- 本组没有运行全量测试，也未写入共享构建。定向 GTest 结果只代表相应刷新后二进制和精确过滤器。
- `BUG-L00-005` 已完成若干目标级链接/用例核查，并静态扫描 39 个最终目标、50 个重复类名和 849 个重复外部符号；`CoverageTests_wtcore` 的 flat-namespace import、弱合并/静态 archive 来源、其余运行时调用与覆盖归属、37/50 类清单对账及 Windows/MSVC 验证仍待完成。
- `BUG-L01-014` 的畸形输入、sanitizer 及配置拒绝路径待产品修复后验证；`BUG-L01-018` 的 Windows/MSVC 回归待后续执行。
- 机器可读的逐条字段、精确证据路径和剩余动作见 `triage/01_share.json`。
