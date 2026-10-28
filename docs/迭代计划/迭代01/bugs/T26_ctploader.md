# T26_ctploader（CTP 数据采集器 TraderSpi/CTPLoader）疑似缺陷

> 状态一律「待审核」；本迭代不修改 `src/**`。行号以 dev 分支当前文件为准。
> 「实测」= 在 `CoverageTests_t_CTPLoader` 里执行过；「嫌疑」= 仅代码级证据，不下结论。

## BUG-T26-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/CTPLoader/TraderSpi.cpp`（`AppendQuery` worker L239-272，尤其 L243-246）+ `src/CTPLoader/TraderSpi.h`（析构 L24-29）
- 相关测试: `src/TestUnits/coverage/t_CTPLoader/t26_ctploader_flow_test.cpp` → `DISABLED_t26_ctploader_flow_hang.destructor_joins_forever_after_queue_drained`
- 输入/前置条件: 触发过一次 `AppendQuery`（登录后队列被 worker 排空），随后析构 `CTraderSpi`。
- 期望行为: `_stopped=true` 后 worker 应在一个睡眠片内退出外层循环，`join()` 正常返回。
- 实际行为: worker 的内层 `while (_queries.empty()) { sleep_for(10ms); }` **不回看 `_stopped`**；队列排空后它永远停在空转里，析构的 `join()` 永不返回。实测打开该 DISABLED 用例 25 秒不退出（kill -9，rc=137）。
- 复现步骤: `--gtest_also_run_disabled_tests --gtest_filter=DISABLED_t26_ctploader_flow_hang.*` 单跑。
- 影响范围: 生产形态下 loader 以 `exit(0)` 收尾、从不调析构，所以日常不炸；但只要 CTraderSpi 被嵌进长命宿主（测试/复用/未来改造为非 exit 收尾），析构即挂死。本轮所有触发队列的启用用例都被迫「泄漏 spi 不析构」，就是这条的绕行成本。
- 备注: 修复方向：内层空转改 `while (!_stopped && _queries.empty())`，并给 `empty()` 加锁/原子化（见 BUG-T26-010）。

## BUG-T26-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/CTPLoader/TraderSpi.cpp`（`fillCzceCode` L318-324）
- 相关测试: 同文件 → `DISABLED_t26_ctploader_flow_crash.fill_czce_code_on_letters_only_scans_past_terminator`
- 输入/前置条件: 任何不含数字的合约代码进入 `fillCzceCode`（例如异常/新形态的 CZCE 代码 "SRCS"）。
- 期望行为: 找不到数字前缀时应判 `'\0'` 终止并原样返回/记日志。
- 实际行为: `for(;;) if (isdigit(code[pid_len++])) break;` 不检查字符串结束符，越过 `'\0'` 继续越界读，直到在进程内存里**碰巧**撞到某个数字字节。实测本机 2ms 返回、拼出的是越界垃圾（不崩，但结果不可复现、不可断言）；页边界布局不同即段错误。
- 复现步骤: 开启 DISABLED 用例单跑（建议只在一次性沙箱里）。
- 影响范围: 当前 CZCE 合约码惯例都带数字，正常柜台回报不命中；一旦上游数据异常（人工夹具、新品种、乱码回报）就是未定义行为，且**锁存的是越界读来的十年位**，污染同进程后续全部 CZCE 代码换算。
- 备注: 修复方向：循环判 `code[pid_len]=='\0'` 提前返回。

## BUG-T26-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/CTPLoader/TraderSpi.cpp`（`LoadFromJson` L662-668 读侧 vs `DumpToJson` L807-813 写侧）
- 相关测试: `t26_ctploader_flow_test.cpp` → `t26_ctploader_flow.incremental_login_reloads_commodities_and_contracts`
- 输入/前置条件: 增量模式（INCREMENTAL=true）用本工具自己 dump 出来的 contracts.json 回读一条期权合约。
- 期望行为: `DumpToJson` 把期权四件套嵌在 `"option":{...}` 子对象里写；`LoadFromJson` 也应在该子对象里读回 optiontype/underlying/strikeprice/underlyingscale。
- 实际行为: 读侧只判 `pCont->has("option")`，随后全部从**合约对象顶层**平铺取值（`pCont->getUInt32("optiontype")` 等）。嵌套键读不到 → optType 落 0（OT_None）、underlying 落 ""、scale 落 0；「dump→增量回读」往返把期权信息整体清零。用例逐字段钉死了这四个值。
- 复现步骤: 见 f6 夹具 JSON（option 块嵌套）；跑该用例即可看到读回值恒为默认。
- 影响范围: 增量刷新流程里所有商品/股指期权合约的期权属性静默丢失；下游按 OT_None 把它当期货处理。
- 备注: 修复方向：`pCont->get("option")` 拿到子对象后在其上取值。

## BUG-T26-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/CTPLoader/TraderSpi.cpp`（`LoadFromJson` L666）
- 相关测试: 同 f6 用例（`EXPECT_NEAR(0.0, cont.m_strikePrice, 1e-9)` 即此钉）
- 输入/前置条件: contracts.json 中合约 `product` 为品种代码字符串（如 "SR"），期权含 `strikeprice`。
- 期望行为: `contract.m_strStrikePrice`（应为 m_strikePrice）读 `"strikeprice"` 键。
- 实际行为: `contract.m_strikePrice = pCont->getDouble("product");` —— 键名写错，把**品种代码字符串**经 `asDouble→strtod` 转成执行价；品种码非数字时静默为 0，数字品种名会得到完全无关的值。
- 复现步骤: f6 用例（"product":"SR" → strike 0，文件里 "strikeprice":5700 被无视）。
- 影响范围: 与 003 同源：增量回读执行价必错；若某品种代码恰以数字开头（如数币类），错误值会伪装成合法执行价，比 0 更难发现。
- 备注: 单行修复；改键名即可。

## BUG-T26-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/CTPLoader/TraderSpi.cpp`（`LoadFromJson` L651-655 无 m_strAltCode 赋值；消费侧 L201-202、L221）
- 相关测试: 同 f6 用例（`EXPECT_TRUE(cont.m_strAltCode.empty())` 钉住现状）
- 输入/前置条件: 增量回读一条 CZCE 合约（dump 侧 L786 明明写了 `"altcode":"SR510"`）。
- 期望行为: `contract.m_strAltCode = pCont->getCString("altcode")`，保持查询柜台时用 CTP 三位原码。
- 实际行为: 装载代码根本不读 altcode。回读后 altCode 为空 → `ReqQryCommission/ReqQryMargin` 的 `InstrumentID` 取换算后四位码 "SR2510"（L201-202 的三元取空侧），CZCE 柜台按原码 "SR510" 认单——**增量模式下所有 CZCE 合约的费率/保证金查询会查不到数据**（静默空回报，不报错）。
- 复现步骤: f6 用例看 `m_strAltCode`；再对照 `ReqQryCommission` 拼 InstrumentID 的分支。
- 影响范围: 只在「增量 + QRYFEES + CZCE」组合命中，但一旦命中是整体性缺数，不是个别合约。
- 备注: 修复方向：装载时读回 altcode；并建议装载后按「code/alt 两键」分别登记以兼容老产物。

## BUG-T26-006

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/CTPLoader/TraderSpi.cpp`（`DumpFees` L711-721）
- 相关测试: `t26_ctploader_dump_test.cpp` → `t26_ctploader_dump.margin_response_dumps_fees_to_fixed_relative_name_with_product_fallback`
- 输入/前置条件: 配置 `feesfile=custom_fees.json`（相对名），QRYFEES 流程走终局 dump。
- 期望行为: 相对 FEES_FILE 应拼到 SAVEPATH 下并**保留配置的文件名**。
- 实际行为: 相对分支硬编码 `path = SAVEPATH; path += "fees.json";`，`FEES_FILE` 的相对名被丢弃（绝对分支 L713-716 才尊重名字）。用例钉了两侧：`fees.json` 存在、`custom_fees.json` 不存在。
- 复现步骤: 见该 fork 用例。
- 影响范围: 配置项 `feesfile` 半失效；同目录跑两个不同配置的 loader 实例会互相覆盖 fees.json。
- 备注: 与 `CTPLoader.cpp` L104/L146/L176 读取该配置的意图不一致，属配置契约缺陷。

## BUG-T26-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/CTPLoader/TraderSpi.cpp`（`DumpFees` L694-699）
- 相关测试: `t26_ctploader_dump_test.cpp` → `DISABLED_t26_ctploader_dump_crash.fees_dump_of_unknown_margin_code_dereferences_end_iterator`
- 输入/前置条件: `_margins` 里存在一个「`_contracts` 没有、`_fees` 既无代码键也无品种键」的合约码（柜台回报了保证金但合约表缺行/品种费率缺失）。
- 期望行为: 两次 find 皆 miss 时跳过该码或记 error。
- 实际行为: 无 end() 判定直接 `const FeeInfo& fInfo = it->second;` —— 解引用 `_fees.end()`，UB。实测本机不崩但落盘的费率行取自越界内存；且该路径终点是 `exit(0)`，宿主进程直接被杀。DISABLED 的原因就是「崩溃形态与落盘值都不可证伪」。
- 复现步骤: 开启 DISABLED 用例（一次性沙箱；注意它必然以 exit(0) 结束进程）。
- 影响范围: 增量/全量场景里保证金与费率回报顺序天然不同步（margin 先回、fee 未回）时完全可能触发；写出的是「看似正常的垃圾费率文件」。
- 备注: 修复方向：miss 时 `continue`；另注意 L694 `_contracts[rawCode]` 的 operator[] 会顺手给合约表插空行（次要，一并评审）。

## BUG-T26-008

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/CTPLoader/TraderSpi.h`（L76 `int _trading_day;` 无初始化）+ `src/CTPLoader/TraderSpi.cpp`（L423、L442 使用）
- 相关测试: 本轮无法为「未初始化值」写可证伪断言（读栈垃圾结果随机）；CZCE 族用例（i13/i14）全部先走替身登录把该值钉成 20251027 再断言，正是绕开此洞
- 输入/前置条件: 不调用 `OnRspUserLogin`（L171 是 `_trading_day` 唯一写点）就收到 CZCE 合约回报。
- 期望行为: 构造函数初始化 `_trading_day`（或在未登录时拒绝换算）。
- 实际行为: 构造函数只初始化 `_stopped(false)`；未登录时 `fillCzceCode(code, _trading_day)` 拿不确定值，十年位/闰十年分支随机。
- 复现步骤: 代码级：`TraderSpi.h` L23 构造 + L76 声明对照 L423 使用点。
- 影响范围: 厂商回调顺序保证「先登录后回报」，正常链路不命中；但顺序假设一旦被流式回放/单测打破，产出的是难复现的错码。
- 备注: 嫌疑级（未证明生产环境可达），修复成本一个等号。

## BUG-T26-009

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/CTPLoader/TraderSpi.cpp`（`LoadFromJson` L637-642、L679）
- 相关测试: f6 用例走的是同一段（行为可跑通，泄漏本身不可断言）
- 输入/前置条件: 增量装载、contracts.json 存在。
- 期望行为: 文件只 load 一次；每个 `load_from_file` 返回的对象恰好 `release()` 一次。
- 实际行为: L639 外层 `root` 判 NULL 后，L642 又对**同一路径再 load 一次**并以内层同名 `root` 遮蔽；L679 `root->release()` 释放的是内层，外层那份整对象（含子节点图）从未释放 → 每次增量装载泄漏一棵配置树。二次 load 也白白多一次磁盘解析。
- 复现步骤: 代码级复读；如评审要证据，可挂 instruments/leak 工具跑一次 f6 场景。
- 影响范围: loader 进程短命（dump 完 exit），实害有限；但和 BUG-T26-001 一样，属于「宿主一复用就暴露」的债。
- 备注: 嫌疑级（未做运行时泄漏计量）；修复方向：删掉 L642 的重复 load。

## BUG-T26-010

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/CTPLoader/TraderSpi.cpp`（`AppendQuery` L230-273：worker 侧 L243 `_queries.empty()`、L258 `front()`、L269 `_queries.size()` 全部**无锁**读；主线程侧 L233-235 emplace 持 `_mtx`）
- 相关测试: 本轮启用用例对「入列后等 worker 消费」的轮询等待（wait_for + 消费计数）即是与此竞态共存的现实成本；竞态本身无启用断言（无法稳定证伪）
- 输入/前置条件: worker 空转期间主线程 `AppendQuery` emplace（QRYFEES 流程：bIsLast 循环连推两条请求必命中该窗口）。
- 期望行为: 对 `std::queue` 的多线程读写全部在同一互斥量下（或改无锁队列/条件变量）。
- 实际行为: `std::queue/std::deque` 非线程安全；worker 无锁读 empty()/front() 与持锁 emplace 并发，ARM64 弱内存序下可读到「size 已更新、节点数据未可见」的中间态。代码级竞态（data race 即 UB）；本 lane 在 stress 中复现过同类形态的堆破坏（发生在夹具的 ostringstream 上，非此队列本身，故此处只登记为嫌疑）。
- 复现步骤: 静态复读 L230-273；动态复现需 TSan 构建，本迭代未授权。
- 影响范围: 与 001 同函数：轻则任务丢失/重排，重则野指针调用。修 001 时一并修最划算。
- 备注: 嫌疑位置明确（L243/L258/L269），不下「已崩」结论。
