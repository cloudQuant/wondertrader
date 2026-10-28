# L02（WTSTools / WtShareHelper / WtMsgQue + 相关 Includes 类型）待审核缺陷

> 状态一律「待审核」；本迭代只记录，不改生产代码。
> 关联测试目录：`src/TestUnits/coverage/l02_wtstools_mq/`

## BUG-L02-001

- 状态: 待审核
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

- 状态: 待审核
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

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/Includes/WTSContractInfo.hpp:233-247（`getLongMarginRatio/getShortMarginRatio`）、:262（`calcFee`）
- 相关测试: `l02_session_contract_test.cpp` → `ContractMarginAndFee`（调用前必定 `setCommInfo`）
- 输入/前置条件: `WTSContractInfo::create()` 后未 `setCommInfo()` 即调用上述三个接口
- 期望行为: 无品种信息时返回合约自身配置或 0
- 实际行为: 直接 `m_commInfo->getMarginRate()` / `m_commInfo->calcFee(...)` 解引用空指针，崩溃
- 复现步骤: `auto* c = WTSContractInfo::create("a","b","E","p"); c->calcFee(1,1,0);`
- 影响范围: 合约与品种加载顺序异常、或期权合约缺品种时，回测/实盘直接崩溃
- 备注: 建议加空指针守卫；本迭代测试规避，不阻塞覆盖率。

## BUG-L02-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/Includes/WTSCollection.hpp:154（`WTSArray::append(obj,false)`）与 :197（`WTSArray::clear()`）
- 相关测试: `l02_variant_collection_test.cpp` → `WtsArrayAppendAtGrabAndSet`（测试里预先 `p2->retain()` 补偿）
- 输入/前置条件: `array->append(obj, false)`（声明容器不持有），随后 `clear()`/`resize()`/容器析构
- 期望行为: 只释放容器真正持有（retain 过）的元素
- 实际行为: `clear()` 无条件对每个元素 `release()`，非持有元素被过度释放，直接变成野指针
- 复现步骤: `WTSArray* a = WTSArray::create(); RefProbe* p = new RefProbe(); a->append(p, false); a->clear(); // p 已被 delete`
- 影响范围: 所有用 `bAutoRetain=false` 借用数组装载外部对象的路径（`getContracts` 一类返回聚合数组的接口最容易踩）
- 备注: 建议容器内记录所有权，或让 `append(..., false)` 存入 `NULL` 占位并跳过释放。

## BUG-L02-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/Includes/WTSCollection.hpp:767（`WTSQueue::pop()`）
- 相关测试: `l02_variant_collection_test.cpp` → `WtsQueuePushPopFrontBack`（断言 pop 后引用计数仍为 2，并手工 release 补偿）
- 输入/前置条件: `q->push(obj)`（自动 retain）后 `q->pop()`
- 期望行为: 出队时对元素 `release()`，与 `push` 的 retain 配对
- 实际行为: `pop()` 只 `_queue.pop_front()`，引用计数不变；元素已从队列消失，后续 `clear()` 也找不到它 → 必然泄漏
- 复现步骤: `WTSQueue* q = WTSQueue::create(); q->push(new RefProbe()); q->pop(); q->release();`（探针对象未析构）
- 影响范围: 消息队列/异步任务队列长时间运行内存持续增长
- 备注: 同类 `WTSStack::pop()` 需要一并核查。

## BUG-L02-006

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/Includes/WTSCollection.hpp:218、:732、:808（`WTSArray/WTSHashMap/WTSQueue::release()`）
- 相关测试: 全部 L02 用例（单线程，未复现）
- 输入/前置条件: 多线程并发对同一容器 retain/release
- 期望行为: 与 `WTSObject::retain()` 的 `fetch_add` 一样使用原子操作
- 实际行为: 覆写的 `release()` 用 `m_uRefs--`（非原子读改写）+ `if (m_uRefs == 0)` 判断，存在丢更新与二次删除窗口；`WTSObject::release()` 用的是 `fetch_sub`，两套语义混用
- 复现步骤: 需要并发压测，本迭代未编写（非确定性）
- 影响范围: 跨线程传递容器对象时的 double free / 泄漏
- 备注: 建议统一为 `fetch_sub` 判定。

## BUG-L02-007

- 状态: 待审核
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

- 状态: 待审核
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

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/WtShareHelper/ShareBlocks.h:15（`BLK_FLAG`）、:54（`SecInfo::_data[1024]`）
- 相关测试: `l02_csv_shareblock_test.cpp` → `ValueTypeSizeTable`/`SectionInfoZeroInitializedAndTypedAccess`
- 输入/前置条件: 拷贝标志位；一个 section 内混放 string 与数值 key
- 期望行为: ①`FLAG_SIZE` 与 `BLK_FLAG` 实际字节数一致，`ShmBlock::_flag` 能容纳；②注释声称的 1024 字节容量对 128 个 key 足够
- 实际行为: ①`BLK_FLAG[] = "&^%$#@!\0"` 的 `sizeof` 为 9，而 `FLAG_SIZE` 为 8，任何用 `sizeof(BLK_FLAG)` 的拷贝都会多写 1 字节到 `char _flag[8]`；②`MAX_KEY_CNT=128` × `SMVT_STRING` 槽位 64 字节 = 8192，远超 `_data[1024]`，仅当全部是数值 key 时才勉强够
- 复现步骤: `static_assert(sizeof(shareblock::BLK_FLAG) == shareblock::FLAG_SIZE, "")` 编译失败
- 影响范围: 共享内存块标志位校验、string 类型 key 较多时的越界写入
- 备注: 需要复核 `allocate_string` 的越界保护（本迭代未测共享内存路径）。

## BUG-L02-013

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/Includes/WTSContractInfo.hpp:37-42、:59（`getCategoty`）
- 相关测试: `l02_session_contract_test.cpp` → `CommodityBasics`
- 输入/前置条件: 读取品种分类
- 期望行为: 接口命名正确（`getCategory`）
- 实际行为: 拼写错误 `getCategoty`；同时 setter/getter 标了 `constexpr`（依赖编译器扩展，标准 C++ 不允许修改成员的函数为 constexpr，`-Wall` 下 clang 接受但 GCC 需扩展开关）
- 复现步骤: 阅读头文件即可
- 影响范围: API 可读性；跨编译器（MSVC/老 GCC）兼容性
- 备注: 改名属于破坏性变更，建议先加别名。
