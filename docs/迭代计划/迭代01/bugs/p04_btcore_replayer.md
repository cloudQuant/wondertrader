# p04_btcore_replayer 缺陷记录（迭代 01 · 第三波）

模块：src/WtBtCore/HisDataReplayer.cpp / .h。初次只读复核生产代码、测试输入、当前样例基础数据及重复登记时未运行测试；后续定向回归已在 BUG-P04-010 记录，本页其余条目的状态仍按各自证据标注。静态分类不等同于动态复现结果；原先把多个不同前提合并进 DISABLED_ 用例的内容，不能作为每一分支已复现的证据。

## BUG-P04-001 缺失 basefiles 时 init 空指针解引用

- 本次裁定：缺失必需配置节点会崩溃的代码路径成立；属于配置校验缺失，不是有效完整配置下的正常路径故障。
- 位置：src/WtBtCore/HisDataReplayer.cpp:171-176,230-235。
- 证据：init() 一进入就解引用 cfg；取得 cfgBF = cfg->get("basefiles") 后未判空便调用 cfgBF->get("session")。因此 cfg == nullptr 和 object 中缺少 basefiles 都没有错误返回路径。
- 测试边界：原禁用复合测试将缺 `basefiles` 错误地断言为初始化成功；现拆为 `initWithoutBasefilesReportsFailure`，在禁用 core dump 的子进程要求安全返回 false。本地定向 FAIL：配置和回放器构造完成、进入 init 后子进程 signal 11；没有栈，具体指令位置仍由源码路径支持。base_cfg() 的生产有效性以存在 basefiles 为前提。
- 修复方向：入口先拒绝 NULL cfg，再拒绝 NULL/非 object basefiles，记录缺失字段后返回 false。负向回归已拆成独立用例 `DISABLED_initWithoutBasefilesReportsFailure`，目前仍禁用且 NOT_RUN；待产品校验修复后启用并断言失败返回。另保留现有 basefiles 内 session/commodity 可选分支测试。

## BUG-P04-002 未 register_sink 时 prepare 解引用空 listener
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）

- 本次裁定：_listener 为空时崩溃的实现事实成立；但“未注册 listener 后调用 prepare”违反了回放初始化前的接收器注册顺序，当前没有证据表明这是有效调用序列，故该禁用测试属于无效前提；入口校验仍是有价值的防御改进。
- 位置：src/WtBtCore/HisDataReplayer.cpp:649-674，尤其 :669；接口入口 src/WtBtCore/HisDataReplayer.h:798-808。
- 证据：prepare() 无条件调用 _listener->handle_init()；头文件将 register_sink() 说明为注册“用于接收回放各类数据和事件”的接收器。没有 listener 时同样无法履行之后的 session/tick/replay-done 回调。
- 测试边界：原禁用复合测试未先调用 register_sink()，该分支已从测试移除；这是夹具未满足回放接收器前置条件。相关复核报告 bugs/R165.md 也明确区分，不能把夹具错误写成有效流程测试结果。本次未运行。
- 修复方向：明确 API 契约。若希望 prepare() 在 sink 缺失时可安全调用，则先检查 _listener，记录错误并返回 false，且不要把 _running 留在 true；测试应断言失败返回。若 sink 是严格调用前置条件，则将其写入 API 文档，并删除声称此用法是合法场景的缺陷断言。

## BUG-P04-003 零结束时间被称为不可控/挂死回放

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）

- 本次裁定：原“循环次数不可控、会挂死”的结论没有源码支持；当前代码对该输入的边界是有限的，但零值的默认语义仍需产品契约确认，不作为已证挂死缺陷。
- 位置：src/WtBtCore/HisDataReplayer.cpp:206-212,649-664,741-760；零日期处理见 src/WTSTools/WTSBaseDataMgr.cpp:673-680。
- 证据：run_by_ticks() 每轮将 _cur_tdate 设为 TimeUtils::getNextDate(_cur_tdate)，循环条件是 _cur_tdate <= end_tdate。若 end 日期为 0，calcTradingDate() 会把零日期替换成当前日期；stime=etime=0 时起止交易日都解析为当前日期，最多推进当前日这一轮后退出。若开始时间有效而结束时间为 0，则截止到当前日期，循环天数可能很大，但有明确的日期上界，不是无界循环。
- 测试边界：当前仍有禁用复合用例 `DISABLED_taskFrequencyAndOutOfRangeReplayCrash`（`p04_replay_termination_test.cpp`），其中零结束时间分支已注册 sink 并调用 `run(false)`，但没有超时或日期推进计数断言，不能证明会挂死。近期配置测试已移除“无 sink 就 prepare”的无效路径；`p04_replayer_config_prepare_test.cpp` 只保留独立禁用的缺失 basefiles 回归。上述测试均未运行。
- 修复方向：产品需明确零值是“默认到当前时间”还是“无效区间”。若必须显式范围，在 init()/prepare() 校验并返回错误；若零表示截至当前，则补有界测试：固定 start/end 输入、记录日期推进次数，并验证回放截止日，不以“会挂死”作期望。


- 迭代02复核: NOT_BUG。零 end time 会按实现解析为当前日期，日循环仍有日期终止条件；没有证据支持旧记录所称无界挂死。禁用 composite 没有 timeout/可观测日期上限，也不能证明 hang。零值代表何种语义可另行澄清，但原挂死结论不成立。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
## BUG-P04-004 空 CSV 日线仍访问首尾元素

- 本次裁定：真实的空数据边界缺陷，属于对存在但无记录的 CSV 输入缺少校验；operator[] 的未定义行为不保证每次都以进程崩溃呈现。
- 位置：src/WtBtCore/HisDataReplayer.cpp:3737-3868，尤其 CSV 分支 :3832-3868。
- 证据：读完表头但无数据行后，barsList->_bars 为空、_count == 0；实现仍读取 _bars[0] 和 _bars[_count - 1]。这是越界访问/未定义行为。外层 get_kline_slice() 捕获 C++ 异常不能把未定义行为转成可靠错误返回。
- 测试边界：原 `DISABLED_emptyCsvCrashes` 缺少期望断言；现改为 `emptyCsvReturnsNullAndReplayCompletesInChild`，在禁用 core dump 的子进程加载 header-only CSV、注册 sink 并 prepare，要求空结果和正常收尾。本地定向 FAIL：阶段标记证明 init 和 prepare 完成、进入 get_kline_slice 后 signal 11；没有栈，不将信号必然归于具体源码行。missingCsvFileYieldsNullSlice 对“不存在的文件”安全早退，与“存在但为空”不是同一前提。
- 修复方向：CSV 读完后先判断记录数；为 0 时清理/移除该 cache 项、记录空文件错误并返回 false，不要生成空 DSB。启用独立测试，断言 get_kline_slice() 返回 nullptr、后续 run() 安全收尾；测试不应要求 UB 必须崩溃。

## BUG-P04-005 空 bars 的 size() - 1 被当成非法下标

- 状态: 已撤回（空 vector 的 UINT32_MAX 在所述调用链中作为未找到哨兵；未发现其被用作 vector 下标）
- 本次裁定：原报告的数值和影响推导不成立；当前空 vector 下 size()-1 再转为 uint32_t 得到 UINT32_MAX，与文档提议的“未找到”标记相同。没有证据证明它被当作合法 vector 下标使用，故不计为独立产品缺陷。
- 位置：src/WtBtCore/HisDataReplayer.cpp:593-631，调用处 :780-783,793-916。
- 证据：idx 是 size_t，函数返回类型是 uint32_t；空 vector 的 0-1 按无符号运算回绕，转为 32 位后为 UINT32_MAX。当 sIdx/eIdx 都是该值时，eIdx-sIdx+1 在 32 位无符号运算下为 1，而非巨大分母；空 BarsList 的 cursor 初值为 UINT32_MAX，run_by_bars() 走 No back data initialized 分支，不读取 _bars[UINT32_MAX]。
- 测试边界：DISABLED_emptyCsvCrashes 先在 BUG-P04-004 的 CSV 首尾读取处遇到 UB；不能证明本条。对空 bars 的有效 loader 输入路径和 locate_barindex() 的其它调用者，本次没有找到独立复现证据。
- 修复方向：可以显式写 if (bars.empty()) return UINT32_MAX; 以表达哨兵语义，并要求 run_by_bars() 对 sIdx/eIdx == UINT32_MAX 早退；除非找到会将哨兵当作真实索引的调用者，否则作为健壮性改进，不单列已确认缺陷。

## BUG-P04-006 upper-bound 在 begin() 上自减

- 本次裁定：合法数据区间落在首根 K 线之前时存在迭代器未定义行为，产品边界缺陷成立；原测试使用倒挂区间，不是合适的复现前提。
- 位置：src/WtBtCore/HisDataReplayer.cpp:593-631，尤其 :620-625。
- 证据：lower_bound() 若命中 begin() 且 bUpperBound 为真、首根日期/时间晚于查询上界，:624 无条件执行 it--。对 begin() 迭代器自减是未定义行为。
- 测试边界：invertedTimeRangeStopsBeforeFirstBar（p04_replay_termination_test.cpp:186-213）把 end 设在 begin 之前，不能证明普通的、起止有序但历史数据尚未开始的区间。有效边界例子是数据首 bar 为 20200103、回放区间为 20200101 至 20200102；上界定位仍可能在 begin 自减。本次未运行。
- 修复方向：若 it == begin()，返回明确的“没有上界 bar”哨兵，不得自减；同时 run_by_bars() 在 sIdx/eIdx 为哨兵或 sIdx > eIdx 时安全结束。用有序区间且首 bar 晚于结束日期的用例验证。

## BUG-P04-007 默认任务 session TRADING 在样例中不存在

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）

- 本次裁定：针对当前仓库样例配置的缺陷主张不成立；引用的禁用测试前提已过时。给自定义 sessions 文件配置了不存在的 task session 时，仍有独立的输入校验缺口，但本记录没有证明这属于默认使用路径。
- 位置：src/WtBtCore/HisDataReplayer.h:810-820；src/WtBtCore/HisDataReplayer.cpp:940-946；样例 dist/common/sessions.json:164-185。
- 证据：默认参数和 DEFAULT_SESSIONID 都是 TRADING；当前仓库的 dist/common/sessions.json 明确包含 TRADING。run_by_tasks() 对查不到的自定义 session 未判空这一实现风险存在，但“默认 session 在样例中缺失”与当前文件矛盾。
- 测试边界：DISABLED_taskFrequencyAndOutOfRangeReplayCrash 使用 term_cfg() 装载仓库样例 sessions，默认任务实际查到 TRADING，所以该测试输入不证明报告的空指针前提。其他报告中重复引用该陈旧判断的内容（如 R80/R131）应随证据刷新，不应作为本条独立复现。本次未运行。
- 修复方向：在 register_task() 或 run_by_tasks() 检查 getSession() 结果；自定义 ID 缺失时记录明确错误并拒绝运行。启用测试需明确使用不包含该 ID 的临时 sessions 文件，不能继续声称仓库默认文件缺 TRADING。


- 迭代02复核: NOT_BUG。任务默认 session 是 TRADING，当前样例 sessions.json 明确包含 TRADING；因此“默认值在样例中不存在”不成立。用户自定义未知 session 属于独立无效配置路径。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
## BUG-P04-008 run_by_bars 在 cursor == size 时下标越界

- 状态: 已合并（迭代02：主编号 BUG-P04-010）

- 本次裁定：run_by_bars() 在读 _bars[_cursor] 前只检查 cursor 是否为 UINT32_MAX，缺少上界保护；但原报告“起始区间晚于最后一根 bar 会把 cursor 设为 size”不成立。可达性需要按真实 cursor 初始化边界收窄。
- 位置：src/WtBtCore/HisDataReplayer.cpp:793-810；get_kline_slice() 初始化游标 :2336-2410。
- 证据：run_by_bars() 在 :796 只排除 UINT32_MAX，:800/:803 随即访问 _bars[_cursor]，之后的 :906 上界检查来不及保护本次读取。对空/越界切片，首次 get_kline_slice() 遇到 lower_bound()==end() 会于 :2407-2409 返回 nullptr，并不把 cursor 设成 size，所以原始的“回测开始晚于所有行情”论据不成立。一个静态可达边界是：只有一根 bar，开始时间在该 bar 日期开盘前；get_kline_slice() 的日线定位得到 eIdx==0，不回退并设 _cursor=eIdx+1==bars.size()==1，随后 run 入口会访问 _bars[1]。该路径未动态运行。
- 测试边界：DISABLED_taskFrequencyAndOutOfRangeReplayCrash 中的 out-of-range 输入在 get_kline_slice() 返回空切片后 cursor 保持哨兵，不能证明越界访问。本次未运行。
- 修复方向：在任何 _bars[_cursor] 读取前验证 _cursor < _bars.size()；同时修正 get_kline_slice() 对单根/当前 bar 的 cursor 语义，避免将仍需回放的 bar 当作已消费。新增单根日线、开始日期等于首 bar 且未收盘的测试；另保留区间晚于最后一根 bar 的安全结束测试。


- 迭代02复核: DUPLICATE。声称的 cursor=size/OOB 情形与 BUG-P04-010 起始首日之前定位所设 cursor=eIdx+1 是同一段定位规则、同一边界根因；单 bar 数据可能成为 _bars[size]，多 bar 情形则跳过首 bar。当前 CSV 测试 oracle 已修正为覆盖所有 bars。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
## BUG-P04-009 数据装载失败后仍登记主 K 线

- 本次裁定：真实的失败路径状态污染可导致 run 崩溃，成立。
- 位置：src/WtBtCore/HisDataReplayer.cpp:2132-2141,2287-2295,677-720,771-777。
- 证据：get_kline_slice(..., isMain=true) 在加载前就写 _main_key/_main_period。CSV/DSB/loader 无数据时函数返回空切片，或时段解析失败时在 :2291-2294 返回；此前写入的 _main_key 没有回滚。之后 run() 只看 _main_key 是否为空便进入 run_by_bars()，其 BarsListPtr barsList = _bars_cache[_main_key] 可能得到空指针，随后 barsList->_code 解引用。故缺失主 K 线的测试若以 isMain=false 调用，不覆盖该路径。
- 测试边界：missingCsvFileYieldsNullSlice（p04_replayer_csv_replay_test.cpp:344-365）刻意使用 isMain=false，只证明无主 K 线时的安全收尾；本次未运行失败加载后的 isMain=true 路径。
- 修复方向：只有在数据、商品 session 和切片装配全部成功后才提交 _main_key/_main_period；失败时不得遗留主周期状态。run_by_bars() 再检查 cache 项非空、bar 列表有效，失败时记录错误并完成安全收尾。测试以缺失 CSV 且 isMain=true 调用，随后验证 run 不崩且回报明确失败/无主数据。

## BUG-P04-010 起始日首根日线未进入回放

- 本次裁定：回放起点等于首 bar 日期且尚未收盘时，游标会跳过首根日线；产品结果缺陷成立。当前 lane 测试把这个行为当成期望，测试 oracle 错误。
- 位置：src/WtBtCore/HisDataReplayer.cpp:2297,2336-2391，后续由 run_by_bars() 按 _cursor 取下一根。
- 证据：日线下，起点为首 bar 日期、起点早于收盘时，lower_bound 命中首 bar，eIdx == 0。:2361-2368 仅在 eIdx > 0 时回退，首 bar 留在当前定位；随后 :2384-2390 因 curBar.date == _cur_tdate 不满足未来日期兜底，把 _cursor 设为 eIdx + 1 == 1。run_by_bars() 从 _bars[_cursor] 开始，首 bar 没有进入闭盘回调。
- 测试前提：dailyBarsReplayFullTradingDayCycle（p04_replayer_csv_replay_test.cpp:205-258）用 5 根日线、起点 20200102 09:30，断言并注释只闭盘后 4 根。该测试观察到了实现行为，但把漏日作为 oracle；csvBarsTransferedToDsbAndReRead 与 reset 用例也沿用 kBarCnt-1，因此这些通过结果不代表回放结果正确。本次未运行。
- 后续定向验证（2026-10-29）: 已将相关用例改为断言窗口内首尾日与收盘价。P04 测试根目录原少上溯一级，导致 session 加载失败；修复路径并重新构建后，三条 P04 和两条 R43 单独运行均到达回放断言并 FAIL：第一根实为 20200103 而非 20200102。主控独立复跑 `dailyBarsReplayFullTradingDayCycle` 也观察到预期 5 根、实得后 4 根。
- 修复方向：明确起点为 bar 当日、尚未收盘时应将第一根纳入后续 close 流程，并兼容起点早于首个有行情日期（不把未来首根丢失）。将回归期望改为区间内 5 根 bar 均闭盘，并断言日期、价格、session/schedule 次数与 bar 数一致。此问题在 q02_btcore3.md、R80.md 等也有同源引用，保留 P04 为根因编号，其他文档应交叉引用而非重复计数。

## BUG-P04-011 回放时间数字格式与头文件注释不一致

- 本次裁定：公开头文件的时间格式说明与实现/仓库用法存在契约冲突；当前证据确认 12 位解析行为，但还不能确定应改实现还是改文档，不能把两种选择同时当成产品错误。
- 位置：src/WtBtCore/HisDataReplayer.h:777-786,887-901；解析 src/WtBtCore/HisDataReplayer.cpp:206-212,661-663,744-746。
- 证据：头文件称 set_time_range 接收 14 位 YYYYMMDDHHMMSS，实现和已有测试均按 12 位 YYYYMMDDHHMM 处理：_cur_date = stime/10000、_cur_time = stime%10000。例：照注释传 20200102093000 会得到日期 2020010209、时间 3000。另 get_raw_time() 的头文件注释称 HHMMSS，但实现与 get_min_time() 一样返回 _cur_time（HHMM），秒数单独由 get_secs() 返回。p04_replayer_config_prepare_test.cpp:12-14,180-200,222-227 明确使用 12 位和 HHMM 值，是项目当前用法证据，不是 14 位行为测试。
- 本机证据与边界：`R134BtPorterBars` 拆分后，12 位窗口的 CTA/SEL/HFT bars 回调逐例 PASS；按公开 14 位注释设置相同窗口的 `DocumentedFourteenDigitRangeReturnsLoadedBars` 单例 FAIL，loader 已调用但返回 0 条。该结果证明当前实现与公开注释冲突，不证明外部调用者实际使用何种格式；尚未遍历项目外消费者。
- 修复方向：由 API owner 确认唯一格式。若兼容当前仓库及 C API 用法，统一修正文档为 12 位 YYYYMMDDHHMM、get_raw_time() 为 HHMM，并说明秒由 get_secs() 提供；若 14 位是已发布契约，则实现 14 位解析与 HHMMSS 暴露、审计所有 HHMM 算术调用点并加兼容/校验测试。无论选哪种，都拒绝悄然错位的输入。
