# 缺陷记录 — lane L03（src/Includes/）

状态一律「待审核」，本迭代不修改任何生产代码。

## BUG-L03-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Includes/WTSDataDef.hpp` L1320 / L1337（`WTSHisTickData::create`）
- 相关测试: `src/TestUnits/coverage/ws03_includes_data/ws03_data_def_test.cpp` → `ws03_his_tick_data.create_and_append_with_factor`（L750 是四实参调用；L773 是显式 `static_cast<bool>(false)` 的两实参调用）
- 输入/前置条件: `WTSHisTickData::create("DCE.i2405");`（只传代码，其余全部用默认参数）
- 期望行为: 唯一可解析的重载，或接口本身不产生歧义
- 实际行为（静态推断，未实编）: 两个重载 `create(const char*, unsigned int nSize = 0, bool = false, double = 1.0)` 与 `create(const char*, bool bValidOnly = false, double = 1.0)` 在只给第一个实参时都可行，单实参调用预计产生 `call to 'create' is ambiguous`。
- 复现步骤（待执行）: 用独立最小翻译单元直接写 `WTSHisTickData::create("DCE.i2405")`，再用项目实际 include 路径及 C++ 标准编译；现有测试文件没有这个裸单实参调用，不能作为该错误的现成复现。
- 影响范围: 任意裸单实参调用都会有编译期歧义；仓内当前调用点均传入额外参数，未找到现行构建受阻证据。API 可用性缺陷仍存在。
- 备注: 当前测试侧用 `create("DCE.i2405", static_cast<bool>(false))` 锚定 bool 重载；这不是单实参复现。建议修复方向：给两个重载改名（如 `create_with_capacity`），或调整默认参数避免实参集合重叠。

### 2026-10-29 复核结论

- 分类：公开 API 的条件性编译缺陷，当前内部调用/测试目标构建失败尚无证据。两个重载均允许单字符串实参，按 C++ 重载规则该调用存在歧义；但仓内 `WtDtMgr.cpp:326` 传入 `(stdCode, false, factor)`，测试 L750 传四个实参，L773 显式传 bool，均未命中该歧义。原“编译该测试文件即可复现”的目标描述不准确，已更正。
- 证据边界：只核对了声明、现有调用与 CMake 配置；本次没有运行 clang、构建、测试或取得失败日志。`src/TestUnits/CMakeLists.txt` 设 C++17；`CoverageTests` 只在 `WT_ENABLE_COVERAGE` 下加入，递归 glob 覆盖该测试源目录。需补真正的单实参 compile-only 用例，并按实际目标记录编译器、标准、include flags 和诊断后，才能称为构建复现。覆盖率状态也未运行，不能据此断言是否阻塞覆盖率。
- 修复方向：明确容量版与普通创建版的 API 形态（改名、标记容量参数类型或去重默认参数），并保留单实参编译检查及现有容量/过滤参数调用检查。

## BUG-L03-002

- 状态: 待审核
- 严重级别: Major（读取未初始化字段；业务影响取决于读取路径）
- 模块/文件: `src/Includes/WTSContractInfo.hpp`（`WTSCommodityInfo` L137-L165、`WTSContractInfo` L321-L356）
- 相关测试: `l03_commodity_info_test.cpp` 中读取业务标量的用例走 `mkComm()` 先写后读；identity/create 用例直接创建但只读取已赋值字符串和有 NSDMI 的手续费算法。`l03_contract_info_test.cpp` 的 `create_and_defaults` 经 `mkContract()` 先写数量字段，未读别称字段。
- 输入/前置条件: `WTSCommodityInfo::create(...)` 之后直接调用 `getVolScale()/getPriceTick()/getCategoty()/getCoverMode()/getPriceMode()/getTradingMode()/getSessionInfo()/getCodes()`；`WTSContractInfo::create(...)` 之后直接调用 `getMaxMktVol()/getMaxLmtVol()/getMinMktVol()/getMinLmtVol()/getAltCode()/getFullAltCode()`
- 期望行为: 未配置字段要么有明确、稳定的默认值，要么 getter 有明确的已初始化前置条件；具体默认值应由 API 契约决定。
- 实际行为: `WTSCommodityInfo` 的构造函数只初始化 `m_nFeeAlg`，`WTSCommodityInfo::create()` 也只写字符数组；`m_uVolScale/m_dPriceTick/m_dLotTick/m_dMinLots/m_ccCategory/m_coverMode/m_priceMode/m_tradeMode/m_pSession` 全部是未初始化的堆内存。`WTSContractInfo` 的 `m_maxMktQty/m_maxLmtQty/m_minMktQty/m_minLmtQty/m_strAltCode/m_strFullAltCode` 同样未初始化（`getAltCode()` 可能返回没有 `'\0'` 终止的字符串）。读这些字段是 UB，`isOption()/isFuture()/isStock()/canShort()/isT1()` 的判定结果不可预测。
- 复现步骤（尚未执行）: 构造对象后立即读取上述未配置字段；应通过 MSan 或确定性初始化检查验证，不能依靠一次读到的随机值判定。
- 影响范围: 直接通过 `create()` 后读取未写字段可能影响交易属性/数量限制/别称解析；仓内基础数据装载流程是否覆盖这些字段需分开核对。
- 备注: 现有测试通过“先写后读”绕过了未初始化路径，尚未验证未配置字段的语义。建议加 NSDMI/构造初始化列表收口已约定的默认值，并单独测试未配置路径；本次未采集行覆盖数据。

### 2026-10-29 复核结论

- 分类：原始 `create()` 对象存在未初始化 getter 字段的 API 风险已确认；产品默认值与实际影响需结合初始化流程判定。`WTSCommodityInfo` 构造函数仅初始化手续费算法，`create()` 写身份字符串；`m_setCodes` 会正常默认构造。`WTSContractInfo` 的日期、保证金率、手续费算法、品种指针、索引和扩展槽已有初始化，但数量限制及别称字符串未初始化。
- 证据边界：`l03_commodity_info_test.cpp:20-34` 的 `mkComm()` 先写入它随后读取的标量；`l03_contract_info_test.cpp:20-34` 的 `mkContract()` 先写数量字段，别称测试先调用 `setAltCode()`，故这些用例规避了未设置路径。另一方面，仓内 `WTSBaseDataMgr.cpp:300-326,361-370` 的品种装载设置了标量并挂接 session；`469-520` 的合约装载设置了别称、品种指针、数量限制、保证金率和放大倍数。当前证据不能说明该基础装载路径会读到未初始化字段，但不能排除直接 API/其他创建路径。未运行 MSan/测试，也没有观察运行时随机值；`getCodes()` 返回的空集合本身是安全的。
- 修复方向：给所有可被 getter 读取的标量、枚举、指针和字符数组设置有明确定义的初值；对必须先配置才能使用的业务字段增加状态/前置条件。补“create 后未 set”的确定性断言和初始化检查，默认枚举值须由接口语义确认，不能只因方便一律置零。

## BUG-L03-003

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定（会话时间合同确认前不定级）
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L454-L467（`isLastOfSection`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.midnight_section_raw_2400_and_0_alias`
- 输入/前置条件: `addTradingSection(2100, 2400)`（即 `second_raw == 2400`）
- 期望行为: 只有收盘时点（2400）判定为小节结束；0000 不应等价
- 实际行为: `endTime = section.second_raw % 2400` 使 raw 收盘值 2400 与查询值 0 混叠，`isLastOfSection(0)` 与 `isLastOfSection(2400)` 均可返回 true。严格模式中的实际影响取决于 `timeToMinutes()` 是否先接受该查询时间，见下方复核。
- 复现步骤: 见测试用例
- 影响范围: 可能影响使用 `isLastOfSection()` 的收盘判定；是否传导到 K 线对齐/行情落盘尚无对应失败证据。
- 备注: 建议方向：只在 `uTime == second_raw` 或（`second_raw == 2400 && uTime == 0`）这类显式跨日映射下匹配，不要用 `% 2400`；先依据时间格式契约决定两者是否应等价。

### 2026-10-29 复核结论

- 分类：`isLastOfSection()` 将 `second_raw` 对 2400 取模的行为由源码确认，且 `midnight_section_raw_2400_and_0_alias` 测试明确断言 0 与 2400 均为末端；这是现有契约/测试选择，不是独立复现的产品故障。应由会话时间格式契约决定 2400 是否允许映射为 0000。
- 更正：该测试只检查 `isLastOfSection()`，没有检查 `isInTradingTime(0, true)`。对文档给出的单段 `2100–2400`，`timeToMinutes(0)` 先返回 `INVALID_UINT32`，`isInTradingTime()` 会在严格末端判断前返回 false；所以原文所述“该输入随之出错”不能由这个例子推出。其他多段/跨日配置是否受影响需要独立有效会话样例验证。
- 证据边界：本次只读了实现和测试断言，没有运行该测试，也未取得 K 线或行情落盘错误日志。修复前先确认生产配置中 `2400`、`0000` 的表示约定，再用包含跨日后续时段的样例验证严格/非严格谓词。
- 修复方向：若两者不应普遍混叠，只做受约束的跨日映射（例如仅对明确的 2400 收盘时点接受 0000），并为单段与跨日多段分别规定末端谓词；若当前行为是兼容约定，则在 API 文档中说明并保留测试。

## BUG-L03-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L103（`setOffsetMins`）与 L78-L101（`addTradingSection`/`setAuctionTime`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.set_offset_mins_leaves_sections_stale`
- 输入/前置条件: 先 `addTradingSection(900, 1500)`，再 `setOffsetMins(300)`
- 期望行为: 修改偏移后各时段的 `first/second`（含集合竞价）应重算，或接口禁止在登记时段后修改偏移
- 实际行为: 时段里缓存的 `first/second` 仍是旧偏移下算出的值，而 `offsetTime()/timeToMinutes()/minuteToTime()` 用的是新 `m_uOffsetMins`，同一对象内部状态自相矛盾（`getOpenTime(true)` 返回 900，而 `offsetTime(900,true)` 返回 1400）。
- 复现步骤: 见测试用例
- 影响范围: 仅当调用方先登记时段、随后改变 offset 并继续使用对象时，派生时间轴会与新 offset 不一致；仓内生产路径尚未找到此顺序。
- 备注: 建议方向：若 setter 允许在登记时段后调用，则重算全部 section；否则改为创建时一次传入并明确时段对象不可变。

### 2026-10-29 复核结论

- 分类：公开可调用的对象状态不一致已由源码确认；当前仓内产品调用路径尚未确认。`addTradingSection()`/`setAuctionTime()` 保存计算后的时间，而 `setOffsetMins()` 仅改偏移值，未重算 trading/auction sections，也未清理 `_minutes` 缓存。
- 证据边界：L03 测试断言旧 section 与新 `offsetTime()` 不一致；仓内 `setOffsetMins()` 调用只检索到测试代码：L02 用例暂时改值后恢复，R12 用例在空时段上改值，暂未发现生产源码调用。未运行测试；不能据此说线上配置热更新已受影响。
- 修复方向：若允许变更已登记对象，重算所有 section 并清空派生缓存；若设计要求构造后不可变，则限制/移除 setter 并在配置装载流程中固定先设偏移、后登记时段。补 trading、auction、`getSecMinList()` 都已缓存后的变更用例。

## BUG-L03-005

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: Major（条件性空指针；是否构成阻断取决于调用前置条件）
- 模块/文件: `src/Includes/WTSContractInfo.hpp` L233-L247（`getLongMarginRatio/getShortMarginRatio`）、L262-L292（`calcFee`）
- 相关测试: `l03_contract_info_test.cpp` → `l03_contract.DISABLED_get_long_margin_ratio_without_comm_info`（DISABLED；未运行，不能作为已复现证据）
- 输入/前置条件: `WTSContractInfo` 未调用 `setCommInfo()`（`m_commInfo == NULL`，构造函数确实置空），随后调用 `getLongMarginRatio()` 且 `m_uMarginFlag != 1`
- 期望行为: 返回合约自身保证金率，或显式返回 0/报错
- 实际行为（源码路径推断，未运行）: 该前置条件下会执行 `m_commInfo->getMarginRate()` 空指针解引用；`calcFee` 在 `m_nFeeAlg == -1` 与 `== 1` 两条路径上也会访问空指针，属于未定义行为，运行时表现尚未验证。
- 复现步骤: 见 DISABLED 用例
- 影响范围: 若调用方在挂接品种前调用，可能触及保证金/手续费路径；仓内产品调用影响尚未确认。
- 备注: 建议方向：明确并执行 `m_commInfo` 的调用前置条件，或定义缺省/失败返回语义；不能在未定义手续费语义前机械地添加同一个早退值。

### 2026-10-29 复核结论

- 分类：条件满足时的空指针解引用已由源码确认；生产影响与 Blocker 定级未证实。构造函数令 `m_commInfo == nullptr`、`m_uMarginFlag == 0`，此时保证金 getter 解引用；`calcFee()` 的算法 `-1` 和算法 `1` 分支也访问品种指针，算法 `0` 分支不访问。现有 DISABLED 测试的前置状态确实走到危险分支，但它没有运行，不能称为已复现崩溃。
- 证据边界：内部 `getLongMarginRatio/getShortMarginRatio` 调用目前只检索到测试；未发现 `WTSContractInfo::calcFee()` 的生产调用点（生产手续费调用检索结果主要是 `WTSCommodityInfo` 指针）。这不排除外部插件/调用方。没有运行测试、崩溃日志或调用栈，也没有确认 API 是否要求调用者先 `setCommInfo()`。
- 修复方向：先明确所有权/初始化契约。若允许未挂接状态，分别为保证金和各手续费算法规定确定结果或错误通道；若不允许，则在构造/装载边界验证必需的品种指针，并将前置条件写入接口文档。添加非 DISABLED 的契约测试，覆盖 margin flag 0/1 及 fee algorithm -1/0/1。

## BUG-L03-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Includes/WTSMarcos.h` L98-L104（`wt_strcpy`）＋ `src/Includes/WTSContractInfo.hpp` L22-L35（`WTSCommodityInfo::create`）
- 相关测试: `l03_commodity_info_test.cpp` → `l03_commodity.identity_field_boundary_length`（只测 63 字符边界，刻意不越界）
- 输入/前置条件: 品种字段长度 ≥ 目标缓冲区容量（`char[64]` 最大安全载荷 63）；合约/别称代码目标为 `char[32]`（最大安全载荷 31），含 32 字节及以上即会越界写终止符或内容。
- 期望行为: 安全截断到缓冲区容量并保留终止符
- 实际行为: `wt_strcpy` 在未传容量时复制完整源串并追加终止符；超出目标容量会越界写。`WTSContractInfo::create` 的 32 字节字段需要 31 字节以内，20 字符代码本身不会越界。
- 复现步骤: 尚未执行；可在隔离 ASan 用例中分别传入 64 字节品种字段和 32 字节合约字段确认，当前没有 ASan 报告。
- 影响范围: 若调用方传入超出目标数组容量的外部/配置字符串，会触发内存安全问题；仓内输入长度边界尚未核实。
- 备注: 建议方向：在每个目标缓冲区调用点传入容量并保证终止符，或使用容量感知的复制接口；若改 `wt_strcpy` 签名需审计全部调用点。现有 63 字节边界测试只验证 `char[64]` 的安全容量，不验证越界行为。

### 2026-10-29 复核结论

- 分类：无容量字符串复制导致越界写的实现风险已确认；是否能从外部数据源触发及产品影响范围未证实。除品种/合约字段外，历史 tick 的 `m_strCode[32]` 也用同一函数接收 `stdCode`。最大安全长度应按各目标数组容量核算；原文“20+ 字符代码临界”错误，已更正为 32 字节缓冲区的 31 字节上限。
- 证据边界：`l03_commodity_info_test.cpp:62-75` 仅测试 63 字符写入 64 字节目标；没有超长输入测试、ASan 运行记录或真实输入长度契约。不能声称 200 字符构造已复现 heap-buffer-overflow。
- 修复方向：改为带容量的复制函数并逐调用点传入 `sizeof(destination)`，保证 NUL 终止；另对外部品种、合约和 tick code 输入实施长度校验。新增刚好容量、超一字符、空串与长字符串的 ASan/边界验证。

## BUG-L03-007

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: Minor
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L191-L243（`minuteToTime`）对比 L295-L330（`secondsToTime`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.minute_to_time_24h_uses_2400_convention`、`l03_session.seconds_round_trip`
- 输入/前置条件: 传入超过交易总时长的分钟数/秒数（例如 225 分钟时段传 4000，或 24 小时时段传 1441）
- 期望行为: 两个反向换算函数对越界的处理一致（要么都 `INVALID_UINT32`，要么都钳位）
- 实际行为: `secondsToTime(越界)` 返回 `INVALID_UINT32`；`minuteToTime(越界)` 返回 `getCloseTime()`，在全天候时段下原始收盘时间是 0，于是越界分钟数静默变成“0000”。调用方拿到 0 无法与真正的 0 点区分。
- 复现步骤: 见测试用例
- 影响范围: 分钟→时间反查（K 线时间轴、绘图刻度）越界时可能画到 0 点。
- 备注: 建议方向：先确定反查越界合同，再决定 `minuteToTime` 是否应改为返回 `INVALID_UINT32` 或保留收盘钳位；同步更新另一反向函数的文档与测试。

### 2026-10-29 复核结论

- 分类：越界策略不一致已由实现和现有测试断言确认；是否为产品缺陷属于 API 合同待确认。`minuteToTime()` 遍历后返回原始 `getCloseTime()`，`secondsToTime()` 返回 `INVALID_UINT32`；L03 测试明确要求普通时段越界钳到收盘，并要求 24 小时越界分钟返回原始收盘 0。因此这不是测试误报，而是既有行为与另一反向函数不同。
- 证据边界：未运行测试，也未检索到越界参数实际进入生产调用的日志或调用点。未找到 API 文档明确要求两函数采用相同策略；“画到 0 点”的影响仍是推断。
- 修复方向：先定下反查越界合同：统一返回 sentinel，或保留钳位并明确区分原始/偏移收盘值；随后同步 `minuteToTime`、`secondsToTime` 文档与边界测试，并核查调用方是否把 0 当有效时间。

## BUG-L03-008

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: Minor
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L144-L189（`timeToMinutes`）＋ L442-L452（`isInTradingTime`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.auction_time_short_circuits_to_zero`
- 输入/前置条件: 至少已添加一个交易时段，再 `setAuctionTime(855, 859)` 后调用 `timeToMinutes(858)` 或 `isInTradingTime(858)`（无交易时段时 `timeToMinutes()` 会先返回 `INVALID_UINT32`）
- 期望行为: 集合竞价不属于连续竞价交易时间（或至少在文档中明确“竞价按 0 分钟计”）
- 实际行为: `timeToMinutes` 在竞价区间直接返回 0（与开盘第一分钟同值），因此在已有交易时段时 `isInTradingTime(858)` 按当前实现返回 true；调用方无法仅凭该返回值区分竞价与开盘分钟。下游是否覆盖数据需另行确认。
- 复现步骤: 见测试用例（断言 `timeToMinutes(858) == 0`）
- 影响范围: 以分钟为下标的行情/指标数组可能出现索引碰撞；是否实际覆盖开盘数据取决于下游写入路径，当前未验证。
- 备注: 建议方向：先明确 `isInTradingTime` 是否包括竞价；若不包括则在谓词中排除，若包括则保留分钟映射并明确下游索引策略。

### 2026-10-29 复核结论

- 分类：竞价映射到分钟 0 是源码注释及现有测试明确接受的行为；竞价是否应被 `isInTradingTime()` 纳入尚无产品约定。按当前实现，配置竞价后 `timeToMinutes(858)==0`，`isInTradingTime(858)` 会因结果不是 `INVALID_UINT32` 而返回 true；L03 测试没有直接断言这个谓词。该索引与开盘分钟 0 相同，但数据是否覆盖由调用方决定。
- 证据边界：仓内存在多个生产 `isInTradingTime()` 调用点（如 `WtEngine.cpp`、ticker、`TraderAdapter.cpp`），但未发现以竞价时刻触发并导致错误交易/落盘的日志或专项测试。没有运行测试。
- 修复方向：明确该谓词是“会话可接收数据”还是“连续竞价时段”；若只指连续交易，应在 auction shortcut 前后独立判定交易 section，必要时让竞价时刻不占分钟下标 0。若竞价应被纳入，则在接口注释和测试中明确 `isInTradingTime(auction)==true` 以及下标冲突处理策略。

## BUG-L03-009

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L217-L219、L230-L231、L321-L322、L357-L358、L489-L490
- 相关测试: 当前没有针对这些回绕分支的用例；公开 API 接受 `int32_t offset` 和 `uint32_t` 时段参数，源码未见取值域校验。
- 输入/前置条件: 试图构造 `section.second > 2400`、`section.first == 0 && section.second == 0`、`getCloseTime(true) == 0`
- 期望行为: 若这些回绕/修正分支确有必要，应有可达路径
- 实际行为: “输出只在 0001..2400”及“分支不可达”并不成立：`offsetTime()` 只回绕一次，且公开 offset/时段参数没有本地范围校验。例：offset=2880、`addTradingSection(900,1000)` 可生成 `3300–3400` 偏移时段，使 `minuteToTime`/`secondsToTime` 的一天回绕条件可满足；offset=-1440 配合 `addTradingSection(0,0)` 可令偏移后收盘为 0，auction 同理可令 `first==0 && second==0`。这些是源码推演，不是已运行复现。
- 复现步骤: 以以上参数增加针对性用例并运行；若这些 offset/HHMM 被产品合同禁止，则应先在创建/变更边界验证合法范围，再仅在该受限域上做可达性分析。
- 影响范围: 原“阻塞 `WTSSessionInfo` 的行/分支 100% 覆盖”判断没有覆盖率报告支持，且所列分支可由无范围限制的公开参数推导为可达；应先补合法输入约束和实测覆盖证据。
- 备注: 不应据当前证据删除这些分支，也不能声称它们阻塞文件 100% 行覆盖。建议先定义并校验 offset 和 HHMM 的合法域；在合同域内证明死分支后再删，或保留并用公开 API 参数测试。没有覆盖率 profile 支撑覆盖率结论。

### 2026-10-29 复核结论

- 分类：原“不可达/死代码”判断为静态前提错误，不是已确认的构建缺陷。`create()`/`setOffsetMins()` 接受任意 `int32_t`，时段和竞价接口接受 `uint32_t`；`offsetTime()` 只加减一次 1440，故超界值可穿透并满足相关条件。对正常交易配置是否可能到达仍取决于未找到的配置取值约束。
- 证据边界：上方参数只用于源码算术推导，本次未运行它们，也没有执行穷举或覆盖率采集。原“穷举”并无命令、结果或日志可核验；不得据此声称 100% 覆盖受阻。
- 修复方向：给 offset 与 `hhmm` 输入定义明确范围并在 API/配置装载边界 fail-fast；随后基于合法域做静态可达性证明和定向测试。若不限制范围，则保留对大偏移的正确归一化逻辑，并对跨日/超一天分支加公开 API 测试。

## BUG-L03-010

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Includes/WTSSessionInfo.hpp` L363-L405（`getTradingSeconds`/`getTradingMins`）
- 相关测试: `l03_session_info_test.cpp` → `l03_session.zero_length_section_is_treated_as_full_day`
- 输入/前置条件: `addTradingSection(900, 900)`（同端点；是否表示零时长、全天或非法配置待合同确认）
- 期望行为: 明确同端点输入代表零时长、全天或非法配置；如果按全天处理，所有时长/分段接口应一致返回 1440 分钟口径。
- 实际行为: 对 `900–900`，当前实现使时长总和为 0 并走 1440 fallback，`getTradingSeconds()` 返回 86400；`getSecMinList()` 对该段仍返回累计值 0，两套口径不一致。
- 复现步骤: 见测试用例
- 影响范围: 若下游用 `getTradingMins/Seconds()` 与 `getSecMinList()` 分别分配/索引，返回不一致可能造成长度或时间轴错配；当前尚无下游故障日志。
- 备注: 建议方向：先明确时段总和为 0 的语义，再让空列表、全天、零时长/非法输入走明确且一致的分支；不应在合同确认前直接把所有 `count == 0` 改为 0。

### 2026-10-29 复核结论

- 分类：返回值不一致已由源码确认；相同端点究竟代表零时长、全天还是非法输入，仍缺配置/API 约定。`getTradingMins/Seconds()` 对任何总和为 0 的时段（包括空列表和 `900–900`）兜底 1440；`getSecMinList()` 对空列表返回 `[1440]`，对 `900–900` 返回 `[0]`。现有测试明确固化了后者，不是未复现的断言。
- 证据边界：未运行测试；没有真实错误配置、下游数组错配或生产日志。代码注释称零总量只能是全天时段，测试又将 `900–900` 按全天计总量，不能据此单方面断言该配置必然是笔误。
- 修复方向：定义全天唯一表示并拒绝/告警其他相等端点，或明确所有相等端点都表示全天；随后让总量函数与 `getSecMinList()` 对空列表、合法全天、零时长/非法输入给出一致结果。补配置解析层的非法值验证和边界测试。
