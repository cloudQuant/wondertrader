# R12 头文件 + 基础工具层 缺陷登记

登记人：lane R12　登记日期：2026-10-27
范围：`src/Includes/WTSDataDef.hpp`、`WTSTradeDef.hpp`、`WTSVariant.hpp`、`WTSSessionInfo.hpp`、`WTSCollection.hpp`、`src/Share/CodeHelper.hpp`、`src/Share/charconv.hpp`、`src/WTSTools/WTSBaseDataMgr.cpp`
本轮 `src/**` 全程只读，未改任何生产代码；以下都是**读源码 + 用例实测**得出的，未重复登记主控已核实的 `getTplIDByPID`（`WTSBaseDataMgr.cpp:880`）与 `isGBK` 死循环两条。

复核备注（2026-10-29）：下文独立复核只读对照源码、测试定义和调用点；本次未运行测试，也未复现原登记中的动态现象。上句“用例实测”保留为原登记人的记录，并非本次独立确认。

---

## BUG-R12-01【高】CodeHelper::find 反向扫描用无符号下标，无该符号的入参必然越界读

- 位置: `src/Share/CodeHelper.hpp:104-133`
- 现象: `for (std::size_t idx = len - 1; idx >= 0; idx--)` —— `std::size_t` 无符号，`idx >= 0` 恒真；当串中不存在目标字符时 idx 从 0 减到 `SIZE_MAX` 并继续读 `src[SIZE_MAX]`，直接越界读/崩溃。
- 影响面: `stdCodeToStdHotCode`(`CodeHelper.hpp:482`)、`stdCodeToStd2ndCode`(`:498`)、`stdCodeToStdCommID2`(`:247`) 对**不含点号**的入参都会触发；`rawFutOptCodeToStdCode`(`:458-466`) 的非 CFFEX 分支是同一写法（`for(; idx >= 0; idx--)`），全数字入参同样触发。
- 证据: 原启用用例 `stdCodeToStdHotCode("BTCUSDT")` / `stdCodeToStdCommID2("600000")` 已因该缺陷移入
  `src/TestUnits/coverage/r12_headers/r12_codehelper_test.cpp` 的 `DISABLED_r12_codehelper.dotless_input_runs_off_the_end_of_the_string`。
- 状态: 已合并（`CodeHelper::find` 归 BUG-L01-001；另一处期权代码扫描归 BUG-L01-014）

## BUG-R12-02【高】WTSQueue::clear 对空槽无判空，NULL 入队后容器清理即崩

- 位置: `src/Includes/WTSCollection.hpp:772-777`（`push` 只在 `obj && bAutoRetain` 时才 retain，允许 NULL 入队）与 `:828-836`（`clear` 直接 `(*it)->release()`）
- 现象: `push(NULL, false)` 后 `size()` 增加，随后任意 `clear()`/`release()` 路径对空指针解引用。
- 证据: `r12_collection_test.cpp` 的 `DISABLED_r12_collection.queue_clear_crashes_on_null_slot`（启用用例里已避免 NULL 入队）。
- 状态: 待审核

## BUG-R12-03【高】WTSVariant::asBoolean 无类型分支，对容器类型把 union 指针当 std::string* 解引用

- 位置: `src/Includes/WTSVariant.hpp:282-290`（对照同族 `asInt32:165`、`asDouble:237`、`asString:255` 都有 `_type` switch）
- 现象: `if (_value._string) { wt_stricmp(_value._string->c_str(), ...) }`。`_value` 是与 `_map`(`ChildrenMap*`) / `_array`(`ChildrenArray*`) 共用的 union（`:712-722`），对 `VT_Object`/`VT_Array` 调用即把容器指针当 `std::string*` 读；尚未找到生产配置解析对嵌套段作此调用的具体位置。
- 附带: `WTSVariant():_type(VT_Null){}`(`:61`) 未初始化 `_value`；但未找到公开工厂正常创建此形态的证据，不能将其作为常规生产触发路径。
- 证据: `r12_variant_more_test.cpp` 的 `DISABLED_r12_variant_refcount.as_boolean_on_container_is_type_confused`。
- 状态: 源码类型混淆确认；生产调用路径及动态后果待验证（主记录，BUG-L02-001 已并入）

## BUG-R12-04【中】WTSQueue::pop 不释放弹出元素，与 WTSStack::pop 语义相反（元素泄漏）

- 位置: `src/Includes/WTSCollection.hpp:767-770`（对照 `WTSStack::pop` `:895-904` 会 `obj->release()`）
- 现象: `push` 自动 retain，`pop` 只 `_queue.pop_front()`，弹出元素引用计数仍是 2，容器与调用方都不再持有可达指针 → 稳定泄漏；另外对空队列调用 `pop()` 是 `deque::pop_front` 前置条件违反。
- 证据: `r12_collection_test.cpp` 的 `DISABLED_r12_collection.queue_pop_does_not_release_element` 期望弹出后引用数为 1，是正确释放语义，并非断言泄漏；此用例尚未运行。同源 BUG-L02-005 的独立正确契约回归已运行并失败。
- 状态: 已合并（主记录 BUG-L02-005；本用例及测试前提保留待复核）

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
- 状态: 已撤回

## BUG-R12-09【低】容器/工具函数用无符号类型返回 -1 作“未找到”

- 位置: `src/Includes/WTSCollection.hpp:92-99`（`WTSArray::idxOf` 返回 `uint32_t`，`return -1` 表达未找到；`idx < 0` 为死条件）；`src/Includes/WTSSessionInfo.hpp:340-346`（`getAuctionStartTime` 空竞价返回同一哨兵）。`WTSVariant.hpp:340-346` 不是所述返回 `-1` 的位置。
- 现象: `-1` 转为 `uint32_t` 得 `UINT32_MAX`，与 `INVALID_UINT32` 一致；当前没有判等错误的证据。无符号 `idx < 0` 属冗余条件；是否有编译告警未验证。
- 证据: 启用用例按当前行为断言（`r12_collection_test.cpp` 断 `INVALID_UINT32`、`r12_session_calc_test.cpp` 断 `(uint32_t)-1`），不改期望值。
- 状态: 已撤回（未发现运行时行为缺陷，保留命名和死条件清理建议）

## BUG-R12-10【低】charconv 的 iconv_open 失败判定写错，失败时仍继续转换

- 位置: `src/Share/charconv.hpp:118-133`（`UTF8toChar`）、`:270-315` 同构的 `ChartoUTF8`
- 现象: `cd = iconv_open("gb2312", "utf-8"); if (cd != 0) { ... }` —— `iconv_open` 的失败值是 `(iconv_t)(-1)` 而不是 0，因此失败描述符仍会进入转换。UTF8toChar 在转换未消耗容量时向 `t_string[dst_len]` 写入，这一位置是所分配区间末尾后一字节；初始化缓冲不能防止该越界。具体平台的转换器与失败路径本次未运行。
- 证据: 启用用例只断言了确定性的“不可转换输入 → 空串”（`r12_charconv_safe_test.cpp`）；中文转换期望值放进了 `DISABLED_r12_charconv_utf8tochar.chinese_conversion_is_platform_dependent`。
- 状态: 源码错误处理和条件越界风险确认；动态复现及平台转换器行为未验证

## BUG-R12-11【低】CodeHelper 多处 `A <= ch && ch <= 'z'` 把方括号等字符当字母

- 位置: `src/Share/CodeHelper.hpp:136-234`（`isStdChnFutOptCode` 状态 2/3）、`:374-443`（`isMonthlyCode`）、`:272-282`（`rawMonthCodeToRawCommID`）
- 现象: `'A'..'z'` 的 ASCII 区间包含 `[ \ ] ^ _ ` `` ` `` 六个非字母字符，`_`（0x5F）恰在其中，于是 `BTC_USDT` 这类代码会被当成“纯字母品种”，`rawMonthCodeToRawCommID("BTC_USDT")` 返回整串而不是截断。
- 说明: 启用用例未把该错误行为写成期望值（`r12_codehelper_test.cpp` 里 `rawMonthCodeToRawCommID` 只喂了 `ag1912`/`rb2405`/`600000`/`BTCUSDT`）。
- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）

## BUG-R12-12【低】WTSTickData::create(const char*) 与 stdCodeToStdHotCode 的缓冲长度计算不闭合

- 位置: `src/Includes/WTSDataDef.hpp:870-878`（`create(stdCode)` 先 `strlen` 再 `memcpy` 后补 `'\0'`）及 `:892-895`（`setCode` 调用 `wt_strcpy`）；两条路径均未校验 `MAX_INSTRUMENT_LENGTH`。CodeHelper 尾部 NUL 子项另见主记录 BUG-L01-002。
- 现象: `len >= MAX_INSTRUMENT_LENGTH` 的输入会越界写，合法输入的最大长度合同仍待确认。CodeHelper 的返回长度问题不在本条重复计数。
- 证据: 短代码用例不覆盖超长输入；R12 的 CodeHelper 用例仅用 `c_str()` 比较，不能检出嵌入尾部 NUL，L01 的完整字符串回归已覆盖该重复子项。
- 状态: 待验证（超长输入条件越界；CodeHelper 子项已并入 BUG-L01-002）

---

## 本轮不进缺陷清单的“已知非缺陷”

- `WTSValueArray::at()` 越界返回 `INVALID_DOUBLE`（`src/Includes/WTSDataDef.hpp:62-74`）是文档化设计，用例按此断言。
- `WTSKlineData::extractData`（`:746-796`）忽略 `head/tail`、永远抓全量数组，与 `WTSKlineSlice::extractData`（`:384-434`）行为不一致。本轮只在启用用例里锁定了**当前**行为（`r12_kline_data.append_bar_overwrites_same_time_and_extacts` 断 `va->size()==1u` 即全量），是否算缺陷请主控裁定，故不在此登记。

## 本轮打不进去的面（非缺陷，属结构性阻塞）

- `src/WTSTools/WTSLogger.cpp`（335 行未覆盖）：`initLogger`/`init` 会 `spdlog::init_thread_pool(8192, 2)` + `flush_every(seconds(2))` + 真实文件 sink（`WTSLogger.cpp:183-243`），与本轮“禁止起线程 / 禁止 sleep / 只写自己临时目录”的门禁直接冲突；模板层 `WTSLogger.h:161-408` 的 `debug/info/warn/error/log_dyn*` 全部要经过这条初始化路径。未测，不计缺陷。
- `src/WTSTools/WTSHotMgr.cpp`（318 行未覆盖）：`loadHots/loadSeconds/loadCustomRules`(`WTSHotMgr.h:39-43`) 需要交易所侧的 hots/seconds JSON，且 `getRawCode/isHot/splitHotSecions`(`:56-82`) 的前置是这些表已装载；同批主力规则解析路径与 R12 头文件层无共享夹具，本轮预算内未建第二套数据面，未测。
- `WTSBaseDataMgr.cpp` 的 `loadSessions/loadCommodities/loadContracts/loadHolidays` 深层分支（`:243-604`）已由 lane R1 的 `r1_basedatamgr_test.cpp` 覆盖，本 lane 只补查询面，避免重复计数。

## 独立复核结论（2026-10-29）

以下“确认”指源码路径与公开方法调用形状足以支持该行为；不代表本次做过动态复现。测试文件中存在断言（尤其是 `DISABLED_` 用例）也不等于该断言本次执行通过。

### BUG-R12-01：确认源码缺陷；与 BUG-L01-001 重复

`CodeHelper::find`（`src/Share/CodeHelper.hpp:104-129`）的反向循环以无符号 `size_t` 判断 `idx >= 0`，无匹配字符时会从 0 回绕并继续越界读取。`stdCodeToStdHotCode` / `stdCodeToStd2ndCode` 虽在调用后检查 `npos`，但无分隔符时该检查到不了。对应 R12 用例 `r12_codehelper_test.cpp:197-202` 为 `DISABLED_`，本次未运行。`rawFutOptCodeToStdCode` 另有同类循环（`:460-465`），但源码注释称当前没有调用方，且 R12 所举全数字输入不属于有效期权代码；不能据此扩大有效输入影响面。建议用边界安全的反向扫描并保留无匹配时返回 `npos`。

### BUG-R12-02：确认 NULL 入队后的容器缺陷；测试未运行

`WTSQueue::push`（`src/Includes/WTSCollection.hpp:772-778`）会把 NULL 存入队列；`front` / `back` 默认 retain 时（`:780-801`）和 `clear`（`:828-836`）均直接解引用槽位。R12 对应用例 `r12_collection_test.cpp:415-421` 是 `DISABLED_`，本次未运行。建议明确契约：若 NULL 不合法则在 `push` 拒绝；若允许，则所有读取和清理路径都需判空。

### BUG-R12-03：确认容器值调用 `asBoolean` 的类型错误；生产配置路径证据不足，且与 BUG-L02-001 重复

`WTSVariant::createObject/createArray`（`src/Includes/WTSVariant.hpp:137-150`）建立容器值，而 `asBoolean`（`:282-290`）不检查 `_type`，直接把 union 的 string 成员当 `std::string*` 使用；对这些公开可创建的容器值调用会触发错误解释。对应测试 `r12_variant_more_test.cpp:291-297` 为 `DISABLED_`，本次未运行。原条目称生产会对嵌套段调用 `getBoolean`，但未定位到这样的具体调用；检索到的 `getBoolean` 调用点使用的是普通配置键，故该影响面未证实。构造函数的 `VT_Null` 路径也没有找到公开工厂，不能把未初始化 null 当作常规公开路径。建议按 `_type` 白名单转换，仅对布尔/字符串执行解析，其他类型返回明确默认值。

### BUG-R12-04：确认 `pop` 丢失队列持有引用；与 BUG-L02-005 重复

`push(obj)` 默认 retain（`src/Includes/WTSCollection.hpp:772-778`），`clear` 会释放队列槽位的引用（`:828-836`）；`front()` 默认另 retain 供调用方持有（`:780-789`）。而 `pop()` 返回 `void`，只调用 `pop_front()`（`:767-770`），既不释放槽位引用，也不能把该引用交给调用方，因此普通 `push` 后 `pop` 会遗失一份引用。`push(obj, false)` 只跳过 retain，但容器清理仍会 release；源码行为要求槽位最终释放一份引用，因此调用方必须确保有一份可释放引用交给队列。是否将 false 明确定义为“所有权移交”仍需 API 文档确认；void `pop` 本身没有把引用传回调用方的能力。测试 `r12_collection_test.cpp:450-460` 被禁用；其中断言弹出后引用数应为 1，表达的是正确释放结果，并非把泄漏行为锁成期望值；该断言本次未执行。建议弹出前取出槽位对象，移除后释放队列持有的一份引用，并写清 false 的所有权规则。空队列 `pop()` 的前置条件尚未由契约/测试确定，应另行定义；当前只确认它直接调用 `deque::pop_front()`。

### BUG-R12-05：确认 NULL 值操作不一致；扩展影响还包括 WTSMap

`WTSHashMap::add`（`src/Includes/WTSCollection.hpp:660-675`）允许存入 NULL；随后 `grab`（`:645-654`）和 `clear`（`:718-726`）无判空解引用。`WTSMap::add`（`:397-412`）也允许 NULL，而 `clear`（`:546-554`）无判空；其 `grab` 已有判空。未找到 R12 对这些 NULL 值路径的运行证据，本次也未运行测试。建议要么拒绝 NULL 插入，要么使 `grab` / `clear` 等所有消费路径一致支持 NULL，并补覆盖。

### BUG-R12-06：确认未初始化 `_count`；与 R1 缺陷清单第 1 项重复

`WTSTickSlice` 构造函数没有初始化 `_count`，`create(code)` 只在 `ticks != NULL` 时设置它（`src/Includes/WTSDataDef.hpp:1393-1424`）；因此空创建后的 `size/empty/at/appendBlock` 读取或累加未初始化值。该创建形式有源码调用点：`src/WtDataStorage/WtRdmDtReader.cpp:391` 和 `src/WtDataStorage/WtDataReader.cpp:2414`。R12 测试 `r12_data_def_test.cpp:571-573` 为禁用用例，本次未运行；R1 已在 `docs/迭代计划/迭代01/bugs/R1_wtdatastorage.md:7-12` 登记。建议在构造函数初始化 `_count(0)`，并保留空创建回归覆盖。

### BUG-R12-07：确认空 K 线切片极值访问缺陷；尚无对应测试

空切片的 `at(0)` 返回 NULL，而 `maxprice/minprice` 首次取值时直接解引用（`src/Includes/WTSDataDef.hpp:326-363`）；同类 `extractData` 对 `_count == 0` 明确返回 NULL（`:384-387`）。现有 R12 用例 `r12_data_def_test.cpp:195-221` 检查空切片的 `at/appendBlock`，`:308-314` 检查空切片的 `extractData`，没有检查空切片的极值调用；本次未运行测试。建议在解引用前处理空切片及无效范围，并明确一致的返回值。

### BUG-R12-08：所报正常边界为误报；零时长区段属于待确认输入边界

`timeToSeconds` 在区段末点将累计区段长度减 1（`src/Includes/WTSSessionInfo.hpp:274-280`），例如 09:30–10:15 的 10:15 映射为 2699，不是 `UINT32_MAX`。已有 `l02_session_contract_test.cpp:308-309` 的断言也将 15:00 的正常收盘点映射到 14399。R12 所述“区段首点就是区段末点”的算术前提不成立。若允许零时长区段，`startSecs == stopSecs` 时才会出现从零减一；当前未找到该配置被支持的证据。建议从正常边界缺陷清单移除该项；如需防御异常时段数据，再单独定义并验证零时长/跨日区段规则。

### BUG-R12-09：非行为缺陷；与 BUG-L02-007 重复

`WTSArray::idxOf()` 的 `return -1` 转成 `uint32_t` 是定义明确的 `UINT32_MAX`，与 `INVALID_UINT32` 哨兵等值；`idx < 0` 对无符号下标确为恒假条件，属于冗余判断。`getAuctionStartTime()` 的无竞价分支同样返回该哨兵。原条目把 `WTSVariant.hpp:340-346` 列为同类 `return -1`，但该处实际是 `getString/getCString` 默认值路径，没有 `return -1`。现有 R12 断言哨兵值（`r12_collection_test.cpp:106-110`、`r12_session_calc_test.cpp:252-253`），本次未运行。建议只做命名清理（显式使用 `INVALID_UINT32`、移除死条件），不要按运行时行为缺陷统计；编译告警主张也未由本次构建验证。

### BUG-R12-10：确认 iconv 错误处理缺陷；测试定义不能证明已复现

POSIX `iconv_open` 的失败哨兵为 `(iconv_t)-1`，源码却以 `cd != 0` 作为成功条件（`src/Share/charconv.hpp:118-128,293-302`），失败后仍调用 `iconv`。在 `UTF8toChar` 中，目标区分配为 `dst_len` 字节；转换未消耗输入时 `dst_len` 仍是容量，随后 `t_string[dst_len] = '\0'` 写到分配区间之外（`:119-127`）。因此原条目“写在缓冲中间”不准确；零字节输出时写到了分配区间末尾后一位。`ChartoUTF8` 同样可能对失败描述符调用 `iconv`，但当前代码在失败输入时先 memset，不能据此照搬 UTF8toChar 的越界结论。启用测试 `r12_charconv_safe_test.cpp:38-50` 的断言和注释未由本次运行验证，且 memset 不会保护上述末尾后一位写。建议按 `(iconv_t)-1` 判失败，保留输出容量与剩余容量，并在任何路径都保证合法终止和明确错误结果。

### BUG-R12-11：字符范围问题成立，但有效字符契约不足，当前例子不构成已证缺陷

`'A'..'z'` 的 ASCII 区间确实包含标点；但 `rawMonthCodeToRawCommID` 注释限定分月期货代码并明确永续码不应传入（`src/Share/CodeHelper.hpp:266-279`），所以 `BTC_USDT` 不能证明该接口对有效输入行为错误。此外现有 L01 测试 `src/TestUnits/coverage/l01_share_wtsutils/l01_codehelper_test.cpp:157-166` 明确期望 `IF_o1` 的前缀为 `IF_o`，说明不能简单把下划线判为非法。建议先从交易所代码规范确定允许字符，再修正范围判断并覆盖允许/拒绝样例；在契约确定前列为待验证，不计确认缺陷。

### BUG-R12-12：WTSTickData 仅能确认超长输入边界风险；CodeHelper 子项重复

`WTSTickData::create(const char*)` 无长度检查地复制并在 `code[len]` 写终止符（`src/Includes/WTSDataDef.hpp:870-877`）；结构体字段为 `code[MAX_INSTRUMENT_LENGTH]`（`src/Includes/WTSStruct.h:151`），容量宏为 32（`src/Includes/WTSMarcos.h:22`），因此当 `len >= 32` 时会越界。`setCode` 也调用不检查目标容量的 `wt_strcpy`（`WTSDataDef.hpp:892-895`、`WTSMarcos.h:98-103`），不能把它当作安全的有界复制。现有测试 `r12_data_def_test.cpp:455-461` 只用短代码，本次未运行，也未找到有效合约码超过该容量的证据，因此只确认超出容量输入会触发的边界风险，不能宣称合法产品输入已触发。建议为两个入口统一校验长度并确定拒绝或截断合同，补最大长度及超长输入测试。

同条中的 `stdCodeToStdHotCode/stdCodeToStd2ndCode` 多留一个 NUL 是源码可确认的问题（`src/Share/CodeHelper.hpp:482-508`），但属于 BUG-L01-002 重复项。原 L01 用例把 `size()==10` 当作预期，现已改为完整字符串和正确长度断言，重建后三例 FAIL；R12 用例仅用 `c_str()` 比较仍会漏掉长度错误。建议修复时将字符串长度设为可见内容长度。
