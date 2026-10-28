# 缺陷记录 — lane L03（src/Includes/）

状态一律「待审核」，本迭代不修改任何生产代码。

## BUG-L03-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Includes/WTSDataDef.hpp` L1320 / L1337（`WTSHisTickData::create`）
- 相关测试: `src/TestUnits/coverage/ws03_includes_data/ws03_data_def_test.cpp` → `ws03_his_tick_data.create_and_append_with_factor`（原 L750）
- 输入/前置条件: `WTSHisTickData::create("DCE.i2405");`（只传代码，其余全部用默认参数）
- 期望行为: 唯一可解析的重载，或接口本身不产生歧义
- 实际行为: 编译错误 `call to 'create' is ambiguous`。两个重载 `create(const char*, unsigned int nSize = 0, bool = false, double = 1.0)` 与 `create(const char*, bool bValidOnly = false, double = 1.0)` 在只给第一个实参时同样可行，字面量 `0`→`unsigned int` 与 `false`→`bool` 的默认实参展开无法区分。
- 复现步骤: `clang++ -std=c++17 -fsyntax-only` 编译该测试文件（门禁命令）
- 影响范围: 所有只按“代码”创建历史 tick 对象的调用点都会踩到编译期歧义；API 可用性缺陷。
- 备注: 本轮在测试侧用 `create("DCE.i2405", static_cast<bool>(false))` 锚定 bool 重载绕过（不改动源码）。建议修复方向：给两个重载改名（如 `create_with_capacity`），或去掉第二个重载的默认参数使其不与第一个重载的默认实参集合重叠。**不阻塞**覆盖率目标（已绕过）。

## BUG-L03-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/Includes/WTSContractInfo.hpp`（`WTSCommodityInfo` L137-L165、`WTSContractInfo` L321-L356）
- 相关测试: `l03_commodity_info_test.cpp` 全部用例（统一走 `mkComm()` 先写后读）；`l03_contract_info_test.cpp` → `l03_contract.create_and_defaults`
- 输入/前置条件: `WTSCommodityInfo::create(...)` 之后直接调用 `getVolScale()/getPriceTick()/getCategoty()/getCoverMode()/getPriceMode()/getTradingMode()/getSessionInfo()/getCodes()`；`WTSContractInfo::create(...)` 之后直接调用 `getMaxMktVol()/getMaxLmtVol()/getMinMktVol()/getMinLmtVol()/getAltCode()/getFullAltCode()`
- 期望行为: 未配置的字段返回确定的默认值（0 / 空串 / nullptr）
- 实际行为: `WTSCommodityInfo` 的构造函数只初始化 `m_nFeeAlg`，`WTSCommodityInfo::create()` 也只写字符数组；`m_uVolScale/m_dPriceTick/m_dLotTick/m_dMinLots/m_ccCategory/m_coverMode/m_priceMode/m_tradeMode/m_pSession` 全部是未初始化的堆内存。`WTSContractInfo` 的 `m_maxMktQty/m_maxLmtQty/m_minMktQty/m_minLmtQty/m_strAltCode/m_strFullAltCode` 同样未初始化（`getAltCode()` 可能返回没有 `'\0'` 终止的字符串）。读这些字段是 UB，`isOption()/isFuture()/isStock()/canShort()/isT1()` 的判定结果不可预测。
- 复现步骤: 构造对象后立即读上述 getter（ASan/MSan 下不稳定）
- 影响范围: 手续费/保证金计算、方向可开性判定（`canShort`）、代码解析（`getAltCode`）；属于资金与下单口径相关，风险高。
- 备注: 阻塞这 6 个 getter 的确定性覆盖（本轮通过“先写后读”绕过，未初始化路径没有断言）。建议加 NSDMI/构造初始化列表收口默认值。**不阻塞**行覆盖，但阻塞这些行的语义正确性。

## BUG-L03-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L454-L467（`isLastOfSection`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.midnight_section_raw_2400_and_0_alias`
- 输入/前置条件: `addTradingSection(2100, 2400)`（即 `second_raw == 2400`）
- 期望行为: 只有收盘时点（2400）判定为小节结束；0000 不应等价
- 实际行为: `endTime = section.second_raw % 2400` 使 2400 与 0 混叠，`isLastOfSection(0)` 与 `isLastOfSection(2400)` 同时为 true。跨日时段（21:00–24:00）在 0 点会误判“小节最后一分钟”，严格模式下的 `isInTradingTime(0, true)` 随之出错。
- 复现步骤: 见测试用例
- 影响范围: 收盘状态判定、K 线收盘对齐、依赖 `isInTradingTime(..., true)` 的行情落盘逻辑。
- 备注: 建议方向：只在 `uTime == second_raw` 或（`second_raw == 2400 && uTime == 0`）这类显式跨日映射下匹配，不要用 `% 2400`。**不阻塞**覆盖率。

## BUG-L03-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L103（`setOffsetMins`）与 L78-L101（`addTradingSection`/`setAuctionTime`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.set_offset_mins_leaves_sections_stale`
- 输入/前置条件: 先 `addTradingSection(900, 1500)`，再 `setOffsetMins(300)`
- 期望行为: 修改偏移后各时段的 `first/second`（含集合竞价）应重算，或接口禁止在登记时段后修改偏移
- 实际行为: 时段里缓存的 `first/second` 仍是旧偏移下算出的值，而 `offsetTime()/timeToMinutes()/minuteToTime()` 用的是新 `m_uOffsetMins`，同一对象内部状态自相矛盾（`getOpenTime(true)` 返回 900，而 `offsetTime(900,true)` 返回 1400）。
- 复现步骤: 见测试用例
- 影响范围: 任何“先建时段、后调偏移”的加载顺序（配置热更新、模板复用）都会得到错误的时间轴映射。
- 备注: 建议方向：`setOffsetMins` 里重算全部 section，或改为 create 时一次性传入。**不阻塞**覆盖率。

## BUG-L03-005

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/Includes/WTSContractInfo.hpp` L233-L247（`getLongMarginRatio/getShortMarginRatio`）、L262-L292（`calcFee`）
- 相关测试: `l03_contract_info_test.cpp` → `l03_contract.DISABLED_get_long_margin_ratio_without_comm_info`（按 README 规则 4 保持 DISABLED，未运行）
- 输入/前置条件: `WTSContractInfo` 未调用 `setCommInfo()`（`m_commInfo == NULL`，构造函数确实置空），随后调用 `getLongMarginRatio()` 且 `m_uMarginFlag != 1`
- 期望行为: 返回合约自身保证金率，或显式返回 0/报错
- 实际行为: 无条件 `m_commInfo->getMarginRate()` 解引用空指针（`calcFee` 在 `m_nFeeAlg == -1` 与 `== 1` 两条路径上同样直接解引用 `m_commInfo`），进程崩溃。
- 复现步骤: 见 DISABLED 用例
- 影响范围: 结算/风控的保证金与手续费口径；基础数据尚未挂接完成时的早期调用路径都可能命中。
- 备注: 建议方向：加 `m_commInfo == nullptr` 早退分支。**阻塞**该 DISABLED 用例转绿，但不阻塞其余行覆盖。

## BUG-L03-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Includes/WTSMarcos.h` L98-L104（`wt_strcpy`）＋ `src/Includes/WTSContractInfo.hpp` L22-L35（`WTSCommodityInfo::create`）
- 相关测试: `l03_commodity_info_test.cpp` → `l03_commodity.identity_field_boundary_length`（只测 63 字符边界，刻意不越界）
- 输入/前置条件: 品种 ID / 名称 / 交易所字符串长度 ≥ 目标 `char[64]`（或合约代码 ≥ `char[32]`）
- 期望行为: 安全截断到缓冲区容量并保留终止符
- 实际行为: `wt_strcpy` 是无长度参数的 `memcpy(des, src, strlen(src)) + des[len]='\0'`，超长输入直接越界写堆；`WTSContractInfo::create` 把 32 字节缓冲区当目标，20+ 字符代码已经临界。
- 复现步骤: 本轮未执行（会破坏测试进程）；可用 200 字符 pid 构造复现，ASan 报 heap-buffer-overflow。
- 影响范围: 外部解析器/交易所有义品种列表导入时的内存安全。
- 备注: 建议方向：`wt_strcpy` 增加容量参数或改用 `snprintf`/`strlcpy`。**不阻塞**覆盖率（本轮测试已按边界值规避）。

## BUG-L03-007

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L191-L243（`minuteToTime`）对比 L295-L330（`secondsToTime`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.minute_to_time_24h_uses_2400_convention`、`l03_session.seconds_round_trip`
- 输入/前置条件: 传入超过交易总时长的分钟数/秒数（例如 225 分钟时段传 4000，或 24 小时时段传 1441）
- 期望行为: 两个反向换算函数对越界的处理一致（要么都 `INVALID_UINT32`，要么都钳位）
- 实际行为: `secondsToTime(越界)` 返回 `INVALID_UINT32`；`minuteToTime(越界)` 返回 `getCloseTime()`，在全天候时段下原始收盘时间是 0，于是越界分钟数静默变成“0000”。调用方拿到 0 无法与真正的 0 点区分。
- 复现步骤: 见测试用例
- 影响范围: 分钟→时间反查（K 线时间轴、绘图刻度）越界时可能画到 0 点。
- 备注: 建议方向：`minuteToTime` 循环结束后返回 `INVALID_UINT32`，与 `secondsToTime` 对齐。**不阻塞**覆盖率。

## BUG-L03-008

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L144-L189（`timeToMinutes`）＋ L442-L452（`isInTradingTime`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.auction_time_short_circuits_to_zero`
- 输入/前置条件: `setAuctionTime(855, 859)` 后调用 `timeToMinutes(858)`，或 `isInTradingTime(858)`
- 期望行为: 集合竞价不属于连续竞价交易时间（或至少在文档中明确“竞价按 0 分钟计”）
- 实际行为: `timeToMinutes` 在竞价区间直接返回 0（与开盘第一分钟同值），因此 `isInTradingTime(858)` 为 true；调用方无法从返回值区分“竞价撮合价”和“09:00:00 的第一笔”。
- 复现步骤: 见测试用例（断言 `timeToMinutes(858) == 0`）
- 影响范围: 以分钟为下标的行情/指标数组，竞价数据会覆盖开盘第一分钟。
- 备注: 建议方向：单独返回哨兵值或在 `isInTradingTime` 中排除竞价。**不阻塞**覆盖率。

## BUG-L03-009

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L217-L219、L230-L231、L321-L322、L357-L358、L489-L490
- 相关测试: 无（无法用公开 API 构造触发条件）
- 输入/前置条件: 试图构造 `section.second > 2400`、`section.first == 0 && section.second == 0`、`getCloseTime(true) == 0`
- 期望行为: 若这些回绕/修正分支确有必要，应有可达路径
- 实际行为: `offsetTime(uTime, false)` 的返回值域被限制在 `0001..2400`（`curMinute <= 0` 一律 `+1440`，`>1440` 一律 `-1440`），所以：①`minuteToTime` 的 `desMin >= 1440` 回绕、②`secondsToTime` 的 `desSecs >= 86400` 回绕、③`getCloseTime` 的 `ret == 0 && bOffseted` 修正、④`isInAuctionTime` 的 `first == 0 && second == 0` 跳过，四处均为不可达死代码。
- 复现步骤: 穷举 `addTradingSection/setAuctionTime/addAuctionTime` 的入参组合
- 影响范围: 阻塞 `WTSSessionInfo` 的行/分支 100% 覆盖（这几行永远跑不到）。
- 备注: 建议方向：删除死分支，或让 `TradingSection` 支持直接注入原始偏移值以便单元测试。**阻塞**该文件 100% 行覆盖。

## BUG-L03-010

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L363-L405（`getTradingSeconds`/`getTradingMins`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.zero_length_section_is_treated_as_full_day`
- 输入/前置条件: `addTradingSection(900, 900)`（零时长时段，常见于配置笔误）
- 期望行为: 返回 0 或报错
- 实际行为: `if (count == 0) count = 1440;` 把零时长静默放大成“全天候 24 小时”，`getTradingSeconds()` 返回 86400；`getSecMinList()` 里对应槽位仍是 0，两套口径不一致。
- 复现步骤: 见测试用例
- 影响范围: 按交易总分钟数预分配数组/缓存长度的地方会分配 1440 项，或把非交易时间当成交易时间。
- 备注: 建议方向：只在时段列表为空时才走 1440 兜底，零长时段应显式告警。**不阻塞**覆盖率。
