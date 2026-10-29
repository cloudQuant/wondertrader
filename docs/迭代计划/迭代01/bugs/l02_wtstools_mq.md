# L02（WTSTools / WtShareHelper / WtMsgQue + 相关 Includes 类型）待审核缺陷

> 状态一律「待审核」；本迭代只记录，不改生产代码。
> 关联测试目录：`src/TestUnits/coverage/l02_wtstools_mq/`

## BUG-L02-001

- 状态: 已合并（BUG-R12-03）
- 严重级别: Major
- 模块/文件: src/Includes/WTSVariant.hpp:282（`WTSVariant::asBoolean()`）
- 相关测试: `l02_variant_collection_test.cpp` → `VariantCrossTypeConversions`（只对 VT_String/VT_Boolean 调用 asBoolean，未覆盖容器分支）
- 输入/前置条件: `WTSVariant::createObject()` 或 `createArray()` 后调用 `asBoolean()`
- 期望行为: 非布尔类型返回 false（与 `asInt32/asCString` 一致，先做 `_type` 守卫）
- 实际行为: `asBoolean()` 不判断 `_type`，直接把 union 成员 `_value._map/_value._array` 当作 `std::string*` 读取并调用 `c_str()`，类型双关（UB），实测会崩溃或读到野指针
- 复现步骤: `WTSVariant* o = WTSVariant::createObject(); o->asBoolean();`
- 影响范围: 任何对未知类型成员直接调 `asBoolean()` 的配置解析路径（`getInt32/getString` 家族中唯一没有类型守卫的一个）
- 备注: 修复方向是给 `asBoolean()` 增加与 `asString()` 相同的 `_type` 白名单；不阻塞本迭代覆盖率目标（测试已规避）。

## BUG-L02-002

- 状态: 已合并（BUG-L03-002）
- 严重级别: Major
- 模块/文件: src/Includes/WTSContractInfo.hpp:133-166（`WTSCommodityInfo`）、:313-337（`WTSContractInfo`）
- 相关测试: `l02_session_contract_test.cpp` → `CommodityBasics`/`ContractBasics`（均先显式 set 再读取）
- 输入/前置条件: `WTSCommodityInfo::create(...)` 后立刻读取 `getVolScale()/getPriceTick()/getLotsTick()/getMinLots()/getCategoty()/getCoverMode()/getPriceMode()/getTradingMode()/getSessionInfo()`
- 期望行为: 未设置的字段有确定初值（0 / 空枚举）
- 实际行为: 这些成员既无默认成员初始化，`WTSCommodityInfo` 构造函数也只初始化了 `m_nFeeAlg`，其余全部未初始化（读栈/堆上的脏值）。`WTSContractInfo` 的 `m_maxMktQty/m_maxLmtQty/m_minMktQty/m_minLmtQty` 同样未初始化
- 复现步骤: `auto* c = WTSCommodityInfo::create("X","x","E","S","T"); c->getVolScale();`
- 影响范围: 手续费/保证金/合约乘数计算可能读到随机值；`m_nFeeAlg` 同时出现在构造函数初始化列表和默认成员初始化里（重复且后者被覆盖）
- 备注: 建议统一改为类内默认初始化；测试里刻意未读取未初始化字段以避免 UB。

## BUG-L02-003

- 状态: 已合并（BUG-L03-005）
- 严重级别: Blocker
- 模块/文件: src/Includes/WTSContractInfo.hpp:233-247（`getLongMarginRatio/getShortMarginRatio`）、:262（`calcFee`）
- 相关测试: `l02_session_contract_test.cpp` → `ContractMarginAndFee`（调用前必定 `setCommInfo`）
- 输入/前置条件: `WTSContractInfo::create()` 后未 `setCommInfo()` 即调用上述三个接口
- 期望行为: 无品种信息时返回合约自身配置或 0
- 实际行为: 直接 `m_commInfo->getMarginRate()` / `m_commInfo->calcFee(...)` 解引用空指针，崩溃
- 复现步骤: `auto* c = WTSContractInfo::create("a","b","E","p"); c->calcFee(1,1,0);`
- 影响范围: 合约与品种加载顺序异常、或期权合约缺品种时，回测/实盘直接崩溃
- 备注: 建议加空指针守卫；本迭代测试规避，不阻塞覆盖率。

## BUG-L02-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/Includes/WTSCollection.hpp:767（`WTSQueue::pop()`）
- 相关测试: `l02_variant_collection_test.cpp` → `WtsQueuePushPopFrontBack`（断言 pop 后引用计数仍为 2，并手工 release 补偿）
- 输入/前置条件: `q->push(obj)`（自动 retain）后 `q->pop()`
- 期望行为: 出队时对元素 `release()`，与 `push` 的 retain 配对
- 实际行为: `pop()` 只 `_queue.pop_front()`，引用计数不变；元素已从队列消失，后续 `clear()` 也找不到它 → 必然泄漏
- 复现步骤: `WTSQueue* q = WTSQueue::create(); q->push(new RefProbe()); q->pop(); q->release();`（探针对象未析构）
- 影响范围: 使用 `WTSQueue` 并经常出队的调用者可能持续泄漏；尚未找到该类的生产调用点，不能推断 `WtMsgQue`（独立实现）发生同样泄漏。
- 备注: 出队前保存元素指针，移除后释放容器持有的一份引用，并覆盖空队列和重复元素的语义。`WTSStack::pop()` 有 release，不能直接归为同一泄漏；其返回指针生命周期契约仍需明确。此问题也见 BUG-R12-04。

## BUG-L02-006

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/Includes/WTSCollection.hpp（`WTSArray/WTSMap/WTSHashMap/WTSQueue::release()`）
- 相关测试: 全部 L02 用例（单线程，未复现）
- 输入/前置条件: 多线程并发对同一容器 retain/release
- 期望行为: 用同一次原子递减的旧值判断唯一的最后释放者，与 `WTSObject::release()` 一致。
- 实际行为: `m_uRefs` 是 atomic，`m_uRefs--` 本身是原子操作；问题在于递减和随后的零值读取分开。例如初始为 2 的两个有效引用并发释放，A 递减后暂停，B 递减至 0，再各自读取到 0，可能重复清理/删除。源码支持该竞争形态，尚未用并发测试复现。
- 复现步骤: 需要并发压测，本迭代未编写（非确定性）
- 影响范围: 跨线程传递容器对象时的 double free / 泄漏
- 备注: 建议以 `fetch_sub` 的返回旧值识别最后释放者，保持容器清理只执行一次；同时检查对象生存期和容器内容同步，补充并发回归/TSan 证据。

## BUG-L02-007

- 状态: 已撤回（与 BUG-R12-09 同为无行为缺陷的哨兵命名/死条件问题）
- 严重级别: Minor
- 模块/文件: src/Includes/WTSCollection.hpp:84、:111、:125、:139（`idx < 0`）、:92（`idxOf`）
- 相关测试: `l02_variant_collection_test.cpp` → `WtsArrayAppendAtGrabAndSet`
- 输入/前置条件: 任意下标访问
- 期望行为: 参数为 `uint32_t` 时下界检查无意义应删除；“未找到”用统一哨兵值
- 实际行为: `if (idx < 0 || idx >= _vec.size())` 中 `idx < 0` 恒为 false（无符号比较），属死代码；`idxOf` 用 `return -1`（隐式转 `UINT32_MAX`）表示未找到，与 `INVALID_UINT32` 混用且没有文档
- 复现步骤: 直接阅读源码/`ay->idxOf(NULL)` 返回 0xffffffff
- 影响范围: 可读性与调用方判等逻辑
- 备注: 无行为错误，建议清理。

## BUG-L02-008

- 状态: 已撤回（返回哨兵与 0 点收盘路径符合现有语义；只剩死分支清理）
- 严重级别: Major
- 模块/文件: src/Includes/WTSSessionInfo.hpp:340（`getAuctionStartTime`）、:348（`getCloseTime`）、:507（`offsetTime`）
- 相关测试: `l02_session_contract_test.cpp` → `EmptySessionDefaults`/`OffsetTimeNoOffset`
- 输入/前置条件: ①无集合竞价的模板调用 `getAuctionStartTime()`；②以 00:00 收盘的时段调用 `getCloseTime(true)`
- 期望行为: ①返回 `INVALID_UINT32` 哨兵（与 `timeToMinutes` 等一致）；②0 点收盘的偏移时间按 2400 表达
- 实际行为: ①`return -1;` 依赖隐式转换成 `0xFFFFFFFF`（写法与其余分支不统一，编译器可能给出符号告警）；②`offsetTime(0,false)` 已经把 0000 变成 2400，因此 `getCloseTime()` 里 `if (ret == 0 && bOffseted) ret = 2400;` 的修正分支永不触发（死代码），而 `minuteToTime` 尾部返回的却是该修正值，两处口径不一致
- 复现步骤: `WTSSessionInfo::create("E","e",0)->getAuctionStartTime()`；`addTradingSection(2100,0)` 后读 `getCloseTime(true)` == 2400
- 影响范围: 跨 0 点夜盘时段的时间换算、K 线对齐
- 备注: 建议统一哨兵常量并复核 0 点收盘口径。

## BUG-L02-009

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/Includes/WTSContractInfo.hpp:130、:291（`calcFee` 取整）
- 相关测试: `l02_session_contract_test.cpp` → `CommodityFeeCalculation`
- 输入/前置条件: 手续费金额较大（例如按成交额计费的股指/债券）
- 期望行为: 按币种精度四舍五入，不溢出
- 实际行为: `(int32_t)(ret * 100 + 0.5) / 100.0`，`ret > 2147 万` 时 int32 溢出（UB），负值时四舍五入方向也不对称
- 复现步骤: `comm->setFeeRates(0.001,0.001,0.001,false); comm->calcFee(1e9, 3, 0);`
- 影响范围: 大额合约手续费/保证金估算
- 备注: 建议改用 `std::round(ret*100)/100`。

## BUG-L02-010

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WTSTools/CsvHelper.cpp:161（`check_cell`）与 :117-123（`get_string(int32_t)` 等）
- 相关测试: `l02_csv_shareblock_test.cpp` → `MissingFieldAndColumnDefaults`（只测了列数一致的正常行，越界用例被规避）
- 输入/前置条件: 数据行字段数少于表头（脏 CSV，很常见）
- 期望行为: 越界列返回默认值
- 实际行为: `check_cell` 用 `_fields_map.size()`（表头列数）校验，而真正访问的是 `_current_cells[col]`；当某行列数 < 表头列数时 `col` 通过校验却越界访问 `std::vector`，`operator[]` 不做检查 → UB
- 复现步骤: 写入 `a,b,c\n1,2\n` 后 `reader.next_row(); reader.get_string(2);`
- 影响范围: 所有 `load_from_file` 的行情/合约 CSV 加载（WTSBaseDataMgr、K 线导入）
- 备注: 建议 `check_cell` 改为按 `_current_cells.size()` 判断，并对 `col < 0` 保留检查。

## BUG-L02-011

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/WTSTools/CsvHelper.h:146（`fields()`）
- 相关测试: 未编写（刻意跳过，避免用例间互相污染）
- 输入/前置条件: 同一进程内先后用两个不同 CSV 的 `CsvReader` 调 `fields()`
- 期望行为: 返回当前实例的字段名列表
- 实际行为: 使用函数内 `static std::string s` 缓存，第二次调用直接返回第一个 reader 的结果；且无锁，多线程读同一 reader 会数据竞争；空表头时 `s.substr(0, s.size()-1)` 在 `size()==0` 下会抛/UB
- 复现步骤: 依次对两个不同 CSV 调 `fields()`
- 影响范围: 依赖 `fields()` 做表头校验的导入逻辑
- 备注: 建议改为成员变量按需生成。

## BUG-L02-012

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: Minor
- 模块/文件: src/WtShareHelper/ShareBlocks.h:15（`BLK_FLAG`）、:54（`SecInfo::_data[1024]`）
- 相关测试: `l02_csv_shareblock_test.cpp` → `ValueTypeSizeTable`/`SectionInfoZeroInitializedAndTypedAccess`
- 输入/前置条件: 拷贝标志位；一个 section 内混放 string 与数值 key
- 期望行为: ①`FLAG_SIZE` 与 `BLK_FLAG` 实际字节数一致，`ShmBlock::_flag` 能容纳；②注释声称的 1024 字节容量对 128 个 key 足够
- 实际行为: ①`BLK_FLAG[] = "&^%$#@!\0"` 的 `sizeof` 为 9，而 `FLAG_SIZE` 为 8，任何用 `sizeof(BLK_FLAG)` 的拷贝都会多写 1 字节到 `char _flag[8]`；②`MAX_KEY_CNT=128` × `SMVT_STRING` 槽位 64 字节 = 8192，远超 `_data[1024]`，仅当全部是数值 key 时才勉强够
- 复现步骤: `static_assert(sizeof(shareblock::BLK_FLAG) == shareblock::FLAG_SIZE, "")` 编译失败
- 影响范围: 共享内存块标志位校验、string 类型 key 较多时的越界写入
- 备注: 需要复核 `allocate_string` 的越界保护（本迭代未测共享内存路径）。


- 迭代02复核: NOT_BUG。BLK_FLAG 的 sizeof=9 与 FLAG_SIZE=8 确实不同，但仓内没有活动 sizeof(BLK_FLAG) 拷贝；唯一实现引用是注释掉的比较，数组本身可容纳 7 个字符和 NUL。_data 的 1024 字节容量可能不足以存 128 个字符串，但 make_valid 在分配前检查 offset+len 并拒绝超额，未见越界写。MAX_KEY_CNT 是 metadata 上限，不是保证所有值槽都可分配。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核（越界主张在仓内路径不成立；常量宽度的外部使用边界待确认）
## BUG-L02-013

- 状态: 已撤回（仅 API 命名兼容性清理项；`constexpr` 编译兼容性指控不成立）
- 严重级别: Minor
- 模块/文件: src/Includes/WTSContractInfo.hpp:37-42、:59（`getCategoty`）
- 相关测试: `l02_session_contract_test.cpp` → `CommodityBasics`
- 输入/前置条件: 读取品种分类
- 期望行为: 接口命名正确（`getCategory`）
- 实际行为: 拼写错误 `getCategoty`；同时 setter/getter 标了 `constexpr`（依赖编译器扩展，标准 C++ 不允许修改成员的函数为 constexpr，`-Wall` 下 clang 接受但 GCC 需扩展开关）
- 复现步骤: 阅读头文件即可
- 影响范围: API 可读性；跨编译器（MSVC/老 GCC）兼容性
- 备注: 改名属于破坏性变更，建议先加别名。

## 2026-10-29 独立复核结论

本节只核对 BUG-L02-001/002/003/007-013；BUG-L02-004 已撤回，BUG-L02-005/006 保留既有复核结论。以下结论来自源码、仓内调用和测试定义的静态检查；本次未运行测试、构建、Sanitizer，也未把待执行的复现步骤写成已复现结果。

### BUG-L02-001：容器类型转换危险成立；影响描述需收窄；与 BUG-R12-03 重复

- 结论：`VT_Object`/`VT_Array` 的 `_value` 当前活动成员分别是 `ChildrenMap*`/`ChildrenArray*`（`WTSVariant.hpp:137-150`），`asBoolean()` 却无 `_type` 检查地以 `std::string*` 调用 `c_str()`（`:282-290`），对这两种公开可创建的类型存在类型混淆和未定义行为。候选中“任意非布尔类型都危险”过宽：数值、字符串、布尔标量目前也都以 `std::string*` 存储（例如 `create(bool)`：`:128-133`）；具体标量转换语义需另按 API 定义。未确认 `VT_Null` 是可由公开工厂正常创建的路径。
- 重复项：与 `R12_headers_wtstools.md` 的 BUG-R12-03 是同一处实现问题，建议合并统计，避免重复计数。
- 证据边界：L02 `VariantCrossTypeConversions` 仅覆盖标量；R12 对容器路径记有 DISABLED 用例。本次未运行或触发崩溃；没有找到嵌套配置生产调用证据，因此不能沿用“生产配置会传入容器值”的影响断言。
- 修复方向：依据 `_type` 明确允许转换的类型，再读取相应 union 成员；容器类型返回定义好的默认值或错误。补 object/array 调用测试，并保留字符串/布尔及数值标量的既有语义。

### BUG-L02-002：直接创建后的未初始化字段风险成立；与 BUG-L03-002 重复

- 结论：`WTSCommodityInfo::create()` 只写身份字符串，构造函数只显式初始化手续费算法；`:146-159` 的放大倍数、价格/数量字段、枚举和 session 指针没有初值。`WTSContractInfo` 的数量限制及别称字符数组也未初始化（`:326-337`）；直接对这些字段调用 getter 具有未定义/不确定行为风险。反之，`m_setCodes` 会默认构造；`WTSContractInfo` 的日期、保证金率、费率算法、品种指针、索引和扩展槽已有初始化，不应列入未初始化项。构造函数初始化列表与成员默认初始化同时写 `m_nFeeAlg=-1` 是冗余，不是“后者被覆盖”的运行时缺陷。
- 重复项及产品路径：与 `l03_includes.md` 的 BUG-L03-002 重复。仓内基础装载路径 `WTSBaseDataMgr.cpp:300-326,469-520` 会写入这些业务字段；这不能证明所有外部或直接 API 创建路径都遵守相同初始化顺序，也没有证据表明该基础路径已触发故障。
- 证据边界：L02/L03 相关测试在读取前先设置字段，未覆盖 `create()` 后直接读取；没有 MSan 或运行时证据。默认枚举和缺省数量值应由接口语义确定，不能据此推定统一置零。
- 修复方向：为所有可能被公开 getter 读取的标量、枚举、指针和字符串定义初始化值；对必需配置字段明确前置条件。补 create 后未设置的契约测试，并检查基础装载和外部调用路径。

### BUG-L02-003：空指针解引用路径成立；调用前置条件与影响级别未证；与 BUG-L03-005 重复

- 结论：构造函数把 `m_commInfo` 置空（`WTSContractInfo.hpp:314-317`）。margin flag 不为 1 时，两个保证金 getter 会解引用该指针（`:233-247`）；手续费算法为 `-1` 或 `1` 时 `calcFee()` 也会使用该指针（`:262-282`），而算法 `0` 路径不依赖它。候选中的无 `setCommInfo()` 调用确实能走到危险源码分支，但是否违反 API 调用前置条件尚未在接口合同中找到答案，不能据此定为 Blocker。
- 重复项及路径：与 `l03_includes.md` 的 BUG-L03-005 重复。`WTSBaseDataMgr.cpp:469-520` 在发布合约前挂接品种；L03 的 DISABLED 用例覆盖的是危险前置状态，但未执行。当前仓内搜索没有确认该非法/未完成初始化顺序进入生产 getter 调用。
- 证据边界：这是源码控制流确认的条件性空指针风险，不是本次运行复现；没有崩溃日志或调用栈，也未证明合法调用方可在未挂接品种时调用这些 getter。
- 修复方向：先写明对象的就绪条件。若未挂接状态允许存在，为 margin 与各 fee 算法定义各自的安全返回/错误语义；若不允许，则在创建/装载边界验证并确保 `setCommInfo()` 先于使用。不要把单一 `0` 作为所有费率路径的未经确认替代语义。

### BUG-L02-007：无符号下界判断是死代码；未发现行为缺陷；与 BUG-R12-09 重复

- 结论：`idx` 为 `uint32_t`，`:84,111,125,139` 的 `idx < 0` 恒假。`idxOf()` 对空指针/未命中返回 `-1`（`:92-105`），转换到 `uint32_t` 得 `UINT32_MAX`；该值与 `INVALID_UINT32`（`WTSMarcos.h:38-44`，32 位平台）相等。因此候选称“哨兵混用造成判等错误”没有源码依据，属于冗余判断与可读性问题。
- 重复项：与 R12 的 BUG-R12-09 中 `WTSArray::idxOf()`/死下界检查部分重复。
- 证据边界：L02 用例及 R12 记录有哨兵断言，但本次未运行；也未验证各目标平台的编译告警。
- 修复方向：可在维护时用 `INVALID_UINT32` 显式表达未命中并删除无符号参数的负数检查；不应按运行时功能缺陷统计。

### BUG-L02-008：所述返回值和 0 点收盘口径未构成行为缺陷；残余是表达清理

- 结论：无竞价时 `return -1` 转成 `uint32_t` 的 `UINT32_MAX`，与 `INVALID_UINT32` 一致；L02 和 L03 相关测试也以该哨兵断言。`addTradingSection()` 在登记时已将结束时间通过 `offsetTime(eTime, false)` 保存；有效 00:00 收盘会向右对齐为 2400（`WTSSessionInfo.hpp:80,507-526`；L02 `OffsetTimeNoOffset` 也断言该行为），故 `getCloseTime(true)` 的 `ret == 0` 修正对该合法路径不会触发。没有证据表明返回的关闭时间与分钟/秒转换产生所称的外部不一致。
- 重复项：无竞价哨兵部分与 R12 BUG-R12-09 重复；死修正分支在 L02 独有条目中，不是已证用户可见故障。
- 证据边界：结论来自转换实现和现有断言，未运行测试。非法时间输入、异常偏移或直接篡改内部缓存不在本结论范围。
- 修复方向：若希望清理，可将 `return -1` 写成 `INVALID_UINT32`，并在确认所有允许输入后移除/注释说明不可达分支；保留现有 00:00 向右对齐语义，除非时间合同另有变更。

### BUG-L02-009：int32 窄化存在可达边界风险；实际业务可达性和负值合同待验证

- 结论：两处 `calcFee()` 都将 `ret*100+0.5` 转成 `int32_t`（`WTSContractInfo.hpp:130,291`）；超过 `int32_t` 可表示范围时，浮点到整数转换不具备定义结果。对正值，`ret` 达到约 21,474,836.475 时，缩放表达式可能越过 int32 边界。候选示例未设置成交额算法所需的 `m_uVolScale`，无法隔离验证此问题；隔离边界示例应先将商品 `setVolScale(1)`，再设成交额费率 0.01 并计算 `price=1e9, qty=3`，其理论 `ret=30,000,000`，缩放后窄化值超出 int32。该数值是否属于产品认可的行情/费率输入仍需确认。负值舍入不对称只有在负手续费、负数量或负价格属于受支持输入时才是合同缺陷。
- 证据边界：源码可确认窄化边界；上述算例仅为静态推导，本次未运行，也没有证明真实交易输入会达到该范围。
- 修复方向：避免先转窄 `int32_t` 再返回 double；采用与手续费精度规则相符的浮点舍入，并对非有限值/允许范围明确处理。补正负值合同、int32 临界值两侧与大值测试；不要在尚未确认负费率语义时改变负向舍入规则。

### BUG-L02-010：短行越界风险成立；与 BUG-R56-003 是同一边界检查根因

- 结论：`check_cell()` 用表头 `_fields_map.size()` 检查索引（`CsvHelper.cpp:161-169`），读值却访问当前行 `_current_cells[col]`（`:77-123`）。表头三列、数据行两列后读取列 2 时会通过当前检查并越过 vector 有效范围；`operator[]` 不提供边界保护。`next_row()` 每行重建 `_current_cells`（`:72-74`），不会补齐短行。
- 重复项：与 R56 BUG-R56-003 共用错误的上界依据，但触发状态不同：R56 是成功读取任何数据行之前 `_current_cells` 为空；L02 是已读到数据但当前短行缺字段。一个按当前行大小检查的修复应同时覆盖两者。关联 R56 用例为 DISABLED，L02 当前用例只验证列数齐全的行。
- 证据边界：源代码足以确认下标可越界；本次没有运行短行输入或启用 R56 用例，也没有 sanitizer 报告。短行如何映射到字段默认值需与 CSV API 约定一致。
- 修复方向：`check_cell()` 同时拒绝负列号，并以 `_current_cells.size()` 为数据访问上限；未成功读取行时返回已有默认值。补短行、中间空字段、尾随空字段及首行读取前调用等用例。

### BUG-L02-011：跨 reader 静态缓存会返回过期字段；影响仅证实到诊断日志

- 结论：`CsvReader::fields()` 使用函数静态 `std::string`，第一次非空 reader 填充后，其他 reader 会直接得到该字符串，而不是自身字段（`CsvHelper.h:146-160`）。仓内已定位到 `HisDataReplayer.cpp:3830` 调用 `reader.fields()`，用途是输出“with fields”日志；数据列读取仍通过当前 reader 的字段映射，因此目前证据只支持日志字段可能错误，不支持导入值本身被该缓存改错。迭代顺序来自 `unordered_map`，也没有顺序稳定保证。
- 更正：空表头时 `s.size()-1` 会无符号下溢为 `npos`，但 `substr(0, npos)` 的位置合法且返回空串，不会因此抛异常或产生越界；原条目的该项指控不成立。并发修改共享静态字符串存在竞态可能，但其触发条件及实际并发使用未验证。
- 证据边界：R56 `BomHeaderParsedAndFieldsExposed` 只测一个 reader，不能发现跨实例污染；本次未执行连续多 reader 用例。产品可见性限定在现已定位的诊断输出。
- 修复方向：把字段串缓存放到 `CsvReader` 实例（加载表头时构造）或改为按值返回；如果字段顺序属于接口合同，应保存原始表头顺序而不是遍历无序映射。补两个不同 header 的 reader 连续调用用例。

### BUG-L02-012：当前拷贝越界与字符串容量主张未被源码支持；存在常量/注释不一致

- 结论：`sizeof(BLK_FLAG)==9` 而 `_flag`/`FLAG_SIZE` 为 8 的算术成立，但仓内 `BLK_FLAG` 唯一非测试引用是 `ShareBlocks.cpp:107` 的注释代码；未找到任何以 `sizeof(BLK_FLAG)` 向 `_flag` 拷贝的活动路径，不能把假设中的拷贝越界记作现存缺陷。`MAX_KEY_CNT` 限制 key 数量，而 `make_valid()` 对新增值另检查 `_offset + len > 1024` 并拒绝（`ShareBlocks.cpp:317-327`）；字符串槽位 64 字节意味着容量先耗尽时分配返回空，不会因为 128 个 key 计数而自然越界。头文件注释对混合类型容量的描述不精确，但并非该越界证据。
- 证据边界：L02 测试只确认常量大小、结构布局及零初始化，没有运行共享内存分配边界；以上结论不覆盖损坏/不可信映射文件中的伪造 `_offset`。
- 修复方向：可去掉字面量中的显式 `\0`，使常量 `sizeof` 与 8 字节标志槽一致；任何将来写 `_flag` 的代码应按明确的 8 字节合同复制并做静态断言。保留 payload 空间检查，并把容量注释改为分别说明 key 上限和字节上限；边界测试应检查分配失败/不越界。

### BUG-L02-013：拼写是兼容性 API 清理项；`constexpr` 编译器不兼容主张不成立

- 结论：`getCategoty` 拼写存在，但生产源码有多处调用（例如 Parser、WtCore 与 `l02_session_contract_test.cpp` 的类别用例），直接改名会破坏源码兼容性。setter/getter 是简单赋值/读取；仓内主要 CMake 目标使用 C++17（例如 `src/CMakeLists.txt:186`、`src/TestUnits/coverage/CMakeLists.txt`），C++14 起放宽的 constexpr 规则允许这类非构造函数成员修改对象；因此“标准 C++ 不允许”及因 GCC 需扩展开关的指控不成立于本项目标准。
- 证据边界：这里只核对源码声明、仓内引用和 CMake 标准设置；未运行跨编译器构建。C++11 独立消费者的兼容性不在仓内标准证据范围。
- 修复方向：若需要纠正命名，保留 `getCategoty()` 作为兼容入口并添加 `getCategory()` 转发别名，待明确主版本/弃用策略后再考虑移除旧 API；无需为了当前 C++17 构建移除 `constexpr`。
