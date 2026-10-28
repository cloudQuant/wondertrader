# R12 头文件 + 基础工具层 缺陷登记

登记人：lane R12　登记日期：2026-10-27
范围：`src/Includes/WTSDataDef.hpp`、`WTSTradeDef.hpp`、`WTSVariant.hpp`、`WTSSessionInfo.hpp`、`WTSCollection.hpp`、`src/Share/CodeHelper.hpp`、`src/Share/charconv.hpp`、`src/WTSTools/WTSBaseDataMgr.cpp`
本轮 `src/**` 全程只读，未改任何生产代码；以下都是**读源码 + 用例实测**得出的，未重复登记主控已核实的 `getTplIDByPID`（`WTSBaseDataMgr.cpp:880`）与 `isGBK` 死循环两条。

---

## BUG-R12-01【高】CodeHelper::find 反向扫描用无符号下标，无该符号的入参必然越界读

- 位置: `src/Share/CodeHelper.hpp:104-133`
- 现象: `for (std::size_t idx = len - 1; idx >= 0; idx--)` —— `std::size_t` 无符号，`idx >= 0` 恒真；当串中不存在目标字符时 idx 从 0 减到 `SIZE_MAX` 并继续读 `src[SIZE_MAX]`，直接越界读/崩溃。
- 影响面: `stdCodeToStdHotCode`(`CodeHelper.hpp:482`)、`stdCodeToStd2ndCode`(`:498`)、`stdCodeToStdCommID2`(`:247`) 对**不含点号**的入参都会触发；`rawFutOptCodeToStdCode`(`:458-466`) 的非 CFFEX 分支是同一写法（`for(; idx >= 0; idx--)`），全数字入参同样触发。
- 证据: 原启用用例 `stdCodeToStdHotCode("BTCUSDT")` / `stdCodeToStdCommID2("600000")` 已因该缺陷移入
  `src/TestUnits/coverage/r12_headers/r12_codehelper_test.cpp` 的 `DISABLED_r12_codehelper.dotless_input_runs_off_the_end_of_the_string`。
- 状态: 待审核

## BUG-R12-02【高】WTSQueue::clear 对空槽无判空，NULL 入队后容器清理即崩

- 位置: `src/Includes/WTSCollection.hpp:772-777`（`push` 只在 `obj && bAutoRetain` 时才 retain，允许 NULL 入队）与 `:828-836`（`clear` 直接 `(*it)->release()`）
- 现象: `push(NULL, false)` 后 `size()` 增加，随后任意 `clear()`/`release()` 路径对空指针解引用。
- 证据: `r12_collection_test.cpp` 的 `DISABLED_r12_collection.queue_clear_crashes_on_null_slot`（启用用例里已避免 NULL 入队）。
- 状态: 待审核

## BUG-R12-03【高】WTSVariant::asBoolean 无类型分支，对容器类型把 union 指针当 std::string* 解引用

- 位置: `src/Includes/WTSVariant.hpp:282-290`（对照同族 `asInt32:165`、`asDouble:237`、`asString:255` 都有 `_type` switch）
- 现象: `if (_value._string) { wt_stricmp(_value._string->c_str(), ...) }`。`_value` 是与 `_map`(`ChildrenMap*`) / `_array`(`ChildrenArray*`) 共用的 union（`:712-722`），对 `VT_Object`/`VT_Array` 调用即把容器指针当 `std::string*` 读；生产里等价于对配置文件的**嵌套段**调用 `config->getBoolean("段名")`。
- 附带: `WTSVariant():_type(VT_Null){}`(`:61`) 从不初始化 `_value`，`VT_Null` 实例走同一分支时读的是未初始化指针。
- 证据: `r12_variant_more_test.cpp` 的 `DISABLED_r12_variant_refcount.as_boolean_on_container_is_type_confused`。
- 状态: 待审核

## BUG-R12-04【中】WTSQueue::pop 不释放弹出元素，与 WTSStack::pop 语义相反（元素泄漏）

- 位置: `src/Includes/WTSCollection.hpp:767-770`（对照 `WTSStack::pop` `:895-904` 会 `obj->release()`）
- 现象: `push` 自动 retain，`pop` 只 `_queue.pop_front()`，弹出元素引用计数仍是 2，容器与调用方都不再持有可达指针 → 稳定泄漏；另外对空队列调用 `pop()` 是 `deque::pop_front` 前置条件违反。
- 证据: `r12_collection_test.cpp` 的 `DISABLED_r12_collection.queue_pop_does_not_release_element`（断言“泄漏”会把错误语义长期锁死，故不进启用用例）。
- 状态: 待审核

## BUG-R12-05【中】WTSHashMap 的 grab/clear 对 NULL 值无判空

- 位置: `src/Includes/WTSCollection.hpp:645-656`（`grab`：`pRet->retain()` 前未判空）、`:718-727`（`clear`：`it->second->release()` 前未判空）
- 现象: `add(key, NULL, false)`（`add` 在 `:660-679` 允许写入 NULL 值）之后，`grab(key)` 或容器清理即空指针解引用；同文件的 `WTSArray::grab`(`:137-146`)、`WTSMap::grab`(`:379-390`) 都有 `if (pRet)` 判空，此处明显是漏写。
- 状态: 待审核

## BUG-R12-06【中】WTSTickSlice 不传 ticks 创建时 _count 未初始化

- 位置: `src/Includes/WTSDataDef.hpp:1390-1424`（成员 `uint32_t _count;` 无默认值，构造函数 `WTSTickSlice(){ _blocks.clear(); }` 只清块表；`create(code)` 的 `ticks==NULL` 分支不写 `_count`）
- 现象: 随后 `size()`/`empty()`/`at()`/`appendBlock()`（`_count += count`）都在读或累加未初始化内存，UB 且断言不可复现。对照 `WTSKlineSlice` 的构造函数 `:215-221` 显式初始化了 `_count(0)`。
- 证据: `r12_data_def_test.cpp` 的 `DISABLED_r12_tick_slice.count_uninitialized_when_created_without_ticks`；启用用例改为先带块创建。
- 状态: 待审核

## BUG-R12-07【中】WTSKlineSlice::maxprice/minprice 对空切片解引用 NULL

- 位置: `src/Includes/WTSDataDef.hpp:326-340`、`:348-363`
- 现象: `double maxValue = this->at(begin)->high;` —— `_count==0` 时 `at()` 在 `:282-284` 直接返回 NULL，这里没有判空也没有像 `WTSValueArray` 那样回落 `INVALID_DOUBLE`；`extractData`(`:384-387`) 有 `_count==0 → NULL` 的保护，两个极值函数漏了。
- 状态: 待审核

## BUG-R12-08【中】WTSSessionInfo::timeToSeconds 在时段末秒返回 UINT32_MAX

- 位置: `src/Includes/WTSSessionInfo.hpp:274-292`
- 现象: 命中时段且 `seconds == stopSecs` 时执行 `offset--`，而 `offset` 是 `uint32_t`，段首即为该段末秒（如日盘第一段 930-1015 的 101500）时 `offset` 还是 0 → 下溢成 4294967295，与“找不到”返回的 `INVALID_UINT32` 无法区分。
- 说明: 启用用例只断言了非下溢的三点（`timeToSeconds(93000)==0`、`(100030)==1830`、`(153000)==INVALID_UINT32`），未把这个下溢值锁进断言。
- 状态: 待审核

## BUG-R12-09【低】容器/工具函数用无符号类型返回 -1 作“未找到”

- 位置: `src/Includes/WTSCollection.hpp:92-99`（`WTSArray::idxOf` 返回 `uint32_t` 却 `return -1`，且 `at`/`grab`/`operator[]` 的 `if(idx < 0 || ...)`(`:82-90`、`:109-116`、`:137-145`、`:163-170`) 里 `idx < 0` 恒假，是死条件）；`src/Includes/WTSSessionInfo.hpp:340-346`（`getAuctionStartTime` 空竞价 `return -1`）；`src/Includes/WTSVariant.hpp:340-346` 同类写法。
- 现象: 语义靠 `0xFFFFFFFF` 表达“非法/未找到”，编译器 `-Wall` 会持续告警，调用方容易漏判。
- 证据: 启用用例按当前行为断言（`r12_collection_test.cpp` 断 `INVALID_UINT32`、`r12_session_calc_test.cpp` 断 `(uint32_t)-1`），不改期望值。
- 状态: 待审核

## BUG-R12-10【低】charconv 的 iconv_open 失败判定写错，失败时仍继续转换

- 位置: `src/Share/charconv.hpp:118-133`（`UTF8toChar`）、`:270-315` 同构的 `ChartoUTF8`
- 现象: `cd = iconv_open("gb2312", "utf-8"); if (cd != 0) { ... }` —— `iconv_open` 失败返回的是 `(iconv_t)(-1)` 而不是 0，因此失败分支也会进入；随后 `iconv()` 直接报错、`dst_len` 不被消费，`t_string[dst_len] = '\0'` 落在缓冲中间（本轮实测靠构造后的 `memset(t_string,0,dst_len)` 才没读出脏数据）。另外 `t_string` 用 `new char[]` 分配却按 `delete[]` 释放是正确的，但 `needFree` 依赖输入是否为纯 ASCII，非 ASCII 分支在 macOS 上没有 gb2312 别名时结果不可预测。
- 证据: 启用用例只断言了确定性的“不可转换输入 → 空串”（`r12_charconv_safe_test.cpp`）；中文转换期望值放进了 `DISABLED_r12_charconv_utf8tochar.chinese_conversion_is_platform_dependent`。
- 状态: 待审核

## BUG-R12-11【低】CodeHelper 多处 `A <= ch && ch <= 'z'` 把方括号等字符当字母

- 位置: `src/Share/CodeHelper.hpp:136-234`（`isStdChnFutOptCode` 状态 2/3）、`:374-443`（`isMonthlyCode`）、`:272-282`（`rawMonthCodeToRawCommID`）
- 现象: `'A'..'z'` 的 ASCII 区间包含 `[ \ ] ^ _ ` `` ` `` 六个非字母字符，`_`（0x5F）恰在其中，于是 `BTC_USDT` 这类代码会被当成“纯字母品种”，`rawMonthCodeToRawCommID("BTC_USDT")` 返回整串而不是截断。
- 说明: 启用用例未把该错误行为写成期望值（`r12_codehelper_test.cpp` 里 `rawMonthCodeToRawCommID` 只喂了 `ag1912`/`rb2405`/`600000`/`BTCUSDT`）。
- 状态: 待审核

## BUG-R12-12【低】WTSTickData::create(const char*) 与 stdCodeToStdHotCode 的缓冲长度计算不闭合

- 位置: `src/Includes/WTSDataDef.hpp:870-878`（`create(stdCode)` 先 `strlen` 再 `memcpy` 后补 `'\0'`，未校验 `MAX_INSTRUMENT_LENGTH`；同族 `setCode` 走 `wt_strcpy` 有截断保护）；`src/Share/CodeHelper.hpp:482-513`（`resize(idx + strlen(SUFFIX_HOT) + 1)` 使返回串尾部多一个 `'\0'`，`size()` 比可见内容长 1）
- 现象: 前者对超长代码溢出写；后者要求调用方只能用 `c_str()` 比较，用 `==` 与 `std::string("DCE.m.HOT")` 比较会不等。
- 证据: 启用用例统一按 `c_str()` 断言（`r12_codehelper_test.cpp` 的 `std_code_to_hot_and_2nd_wrappers`）。
- 状态: 待审核

---

## 本轮不进缺陷清单的“已知非缺陷”

- `WTSValueArray::at()` 越界返回 `INVALID_DOUBLE`（`src/Includes/WTSDataDef.hpp:62-74`）是文档化设计，用例按此断言。
- `WTSKlineData::extractData`（`:746-796`）忽略 `head/tail`、永远抓全量数组，与 `WTSKlineSlice::extractData`（`:384-434`）行为不一致。本轮只在启用用例里锁定了**当前**行为（`r12_kline_data.append_bar_overwrites_same_time_and_extacts` 断 `va->size()==1u` 即全量），是否算缺陷请主控裁定，故不在此登记。

## 本轮打不进去的面（非缺陷，属结构性阻塞）

- `src/WTSTools/WTSLogger.cpp`（335 行未覆盖）：`initLogger`/`init` 会 `spdlog::init_thread_pool(8192, 2)` + `flush_every(seconds(2))` + 真实文件 sink（`WTSLogger.cpp:183-243`），与本轮“禁止起线程 / 禁止 sleep / 只写自己临时目录”的门禁直接冲突；模板层 `WTSLogger.h:161-408` 的 `debug/info/warn/error/log_dyn*` 全部要经过这条初始化路径。未测，不计缺陷。
- `src/WTSTools/WTSHotMgr.cpp`（318 行未覆盖）：`loadHots/loadSeconds/loadCustomRules`(`WTSHotMgr.h:39-43`) 需要交易所侧的 hots/seconds JSON，且 `getRawCode/isHot/splitHotSecions`(`:56-82`) 的前置是这些表已装载；同批主力规则解析路径与 R12 头文件层无共享夹具，本轮预算内未建第二套数据面，未测。
- `WTSBaseDataMgr.cpp` 的 `loadSessions/loadCommodities/loadContracts/loadHolidays` 深层分支（`:243-604`）已由 lane R1 的 `r1_basedatamgr_test.cpp` 覆盖，本 lane 只补查询面，避免重复计数。
