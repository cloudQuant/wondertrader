# 缺陷记录 — lane L01（src/Share + src/WTSUtils）

> 本迭代只记录缺陷，不修改生产代码；状态一律「待审核」，修复需评审后单独提交。
> 对应测试：`src/TestUnits/coverage/l01_share_wtsutils/`
> 编号与测试代码里引用的 `BUG-L01-xxx` 一致。

## BUG-L01-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/Share/CodeHelper.hpp` `CodeHelper::find` (L104-L129)
- 相关测试: 未编写可执行用例（触发即越界读/崩溃，会拖垮整个测试进程）；`l01_codehelper_test.cpp` 只用带 `.` 的输入
- 输入/前置条件: `find("abc", '.', true)`，即反向查找的字符不存在
- 期望行为: 遍历结束后返回 `std::string::npos`
- 实际行为: `for (std::size_t idx = len - 1; idx >= 0; idx--)` 用无符号下标，`idx >= 0` 恒为真；`idx` 从 0 回绕成 `SIZE_MAX` 后继续读 `src[SIZE_MAX]`，越界访问，直到偶然撞上 `.` 或段错误
- 复现步骤: 调用依赖该分支的 `stdCodeToStdHotCode("ABC")` / `stdCodeToStd2ndCode("ABC")` / `stdCodeToStdCommID2("ABC")`
- 影响范围: `stdCodeToStdHotCode`、`stdCodeToStd2ndCode` 里的 `if (idx == npos) return ""` 保护永远不会命中；所有无 `.` 的异常代码都可能崩溃
- 备注: 不阻塞本迭代覆盖率目标。修复方向：改用有符号 `std::ptrdiff_t` 或 `idx-- > 0` 形式（仅提建议，未实施）

## BUG-L01-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Share/CodeHelper.hpp` `stdCodeToStdHotCode` (L482-L493)、`stdCodeToStd2ndCode` (L498-L509)
- 相关测试: `l01_codehelper_test.cpp: std_code_to_std_hot_code / std_code_to_std_2nd_code / std_code_to_std_hot_code_without_suffix_sep`
- 输入/前置条件: `stdCodeToStdHotCode("DCE.m.2109")`
- 期望行为: 返回 `std::string("DCE.m.HOT")`，`size()==9`
- 实际行为: `resize(idx + strlen(SUFFIX) + 1)` 多留一字节，返回串末尾带一个多余的 `'\0'`，`size()==10`；只有 `c_str()` 截断后才看起来正确
- 复现步骤: `EXPECT_EQ(stdCodeToStdHotCode("DCE.m.2109"), std::string("DCE.m.HOT"))` 会失败（长度 10 vs 9）
- 影响范围: 合约码作为 map key、与 `std::string` 直接比较、写文件/网络序列化时出现看不见的尾部 `\0`
- 备注: 测试里断言的是当前实际行为（`size()==10`）加注释；修好后需要把期望改成 9。不阻塞覆盖率

## BUG-L01-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Share/Converter.hpp` `convert::to_str` (L21-L34)
- 相关测试: `l01_converter_charconv_test.cpp: to_str_into_caller_buffer_keeps_stale_tail`、`DISABLED_to_str_into_caller_buffer_should_not_keep_stale_tail`、`DISABLED_to_str_internal_static_buffer_stale_digits`
- 输入/前置条件: 先 `to_str(buf, 32, 123456)` 再 `to_str(buf, 32, 7)`；或先 `to_str<int64_t>(9876543210LL)` 再 `to_str<int32_t>(1)`
- 期望行为: 第二次调用返回 `"7"` / `"1"`
- 实际行为: `std::to_chars` 不写结束符，函数又不清零缓冲区，短数字覆盖后残留上一次长数字的尾巴，返回 `"723456"`；单参版本共用 `thread_local` 缓冲区，跨用例顺序相关
- 复现步骤: 见上述用例
- 影响范围: 所有使用 `convert::to_str` 生成字符串的代码（日志、代码拼接、行情字段格式化）；短数值跟在长数值后面时输出错误数据
- 备注: 修复方向：用 `to_chars` 返回的 `ptr` 写 `*ptr = '\0'`（或返回 `to_chars_result`）。不阻塞覆盖率

## BUG-L01-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Share/charconv.hpp` `URLEncode` (L388-L458)、`URLDecode` (L469-L533)
- 相关测试: `l01_converter_charconv_test.cpp: url_encode_ascii / url_decode_keeps_percent_for_safe_chars`
- 输入/前置条件: `URLDecode("%41")`、`URLDecode("%2f")`；`URLEncode("%41")`
- 期望行为: 百分号编码被完整还原，`"%41"` → `"A"`，`URLEncode` 把 `%` 自身转义为 `%25`
- 实际行为: `URLDecode` 把落在“可不编码白名单”（含 `0x2f`、字母数字等）里的 `%XX` 原样保留成字面量，`URLDecode("%41")=="%41"`；`URLEncode` 遇到 `%` 不转义，直接输出 `%41`
- 复现步骤: `EXPECT_STREQ(static_cast<const char*>(URLDecode("%41")), "A")` 失败
- 影响范围: 含 `%`、`/`、`:`、`?` 等字符的 URL 参数解码结果错误；编解码不可逆、存在歧义
- 备注: 属设计缺陷，建议增加严格模式（所有 `%XX` 一律解码）。不阻塞覆盖率

## BUG-L01-005

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/Share/charconv.hpp` `EncodingHelper::isGBK` (L560-L582)
- 相关测试: `l01_converter_charconv_test.cpp: DISABLED_encoding_helper_is_gbk_hangs`（禁用，触发即挂死）
- 本迭代内的实测事故: 该缺陷类不止 `0xFF`。`{0xD6,0xF7}`（次字节 ==0xF7）与 `{0xD6,0x20}`（次字节 <0x40）同样走失败分支，而它们当时留在**启用态**的 `encoding_helper_is_gbk` 里；`CoverageTests` 因此在 `l01_charconv.encoding_helper_is_gbk` 上空转 9 分 28 秒（86% CPU，日志无 `[ OK ]`），由主控 `kill -TERM` 止损。已把这两条输入并入 `DISABLED_` 用例（仅改测试代码，生产代码零改动），并给 `coverage.sh` 加了 `BIN_TIMEOUT` 挂死看门门限（默认 420s，本次用 240s），会自动报出最后一条无结论的 RUN 用例。
- 输入/前置条件: `isGBK({0xFF, 0xFF}, 2)`；或任何高位字节但双字节校验失败的数据（`0x80`、`>0xFE`、次字节 `<0x40` 或 `==0xF7`）
- 期望行为: 校验失败时推进游标继续扫描，最终返回 `false`
- 实际行为: `else` 分支里 `if(...)` 不成立时既不返回也不 `i++`，`while (i < len)` 死循环；且当高位字节位于最后一个字节时读取 `data[i + 1]` 越界
- 复现步骤: 取消 `DISABLED_` 前缀运行，用例会超时挂死
- 影响范围: 所有编码探测路径（行情/配置文件 GBK 判定），可造成进程挂死或读到越界内存
- 备注: 修复方向：失败分支补 `i += 2`（或 `i += 1`）并先判 `i + 1 < len`。当前该函数的 `false` 分支只能靠纯 ASCII/合法双字节输入覆盖

## BUG-L01-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Share/TimeUtils.hpp` `TimeUtils::getNextMinute` (L299-L313)
- 相关测试: `l01_timeutils_test.cpp: get_next_minute_underflow_returns_wrapped_uint`、`DISABLED_get_next_minute_should_normalize_full_days`
- 输入/前置条件: `getNextMinute(0, -1500)`
- 期望行为: `2300`（等价于回退 60 分钟）
- 实际行为: 只对 `totalMins` 做一次 `+1440`，`-1500` 后仍为 `-60`，`(uint32_t)(-100)` 返回 `4294967196`
- 复现步骤: 见用例
- 影响范围: 分钟线时间戳回拨/跨多天的位移计算会得到天文数字；调用方一般是交易时段推算
- 备注: 修复方向：`totalMins = ((totalMins % 1440) + 1440) % 1440`。不阻塞覆盖率

## BUG-L01-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Share/TimeUtils.hpp` `TimeUtils::getNextMonth` (L319-L335)
- 相关测试: `l01_timeutils_test.cpp: get_next_month_negative_months_is_off_by_one_year`、`DISABLED_get_next_month_negative_months`
- 输入/前置条件: `getNextMonth(202401, -1)`、`getNextMonth(202406, -1)`
- 期望行为: `202312`、`202405`
- 实际行为: `uAddYear = months/12` 对负数截断为 `0`，`uAddMon = months%12` 再 `+12` 变成 `11`，年份不回退，结果 `202412`、`202505`（大一年）
- 复现步骤: 见用例
- 影响范围: 所有做“上月/上上月”推算的月份合约、展期逻辑
- 备注: 修复方向：把月份换算成绝对月序号 `y*12+m + months` 再还原。不阻塞覆盖率

## BUG-L01-008

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/Share/TimeUtils.hpp` `TimeUtils::timeToMinBar` (L337-L340)
- 相关测试: `l01_timeutils_test.cpp: time_to_min_bar_overflows_after_2032`、`DISABLED_time_to_min_bar_should_roundtrip_for_2033`
- 输入/前置条件: `timeToMinBar(20330101, 930)`
- 期望行为: `4301010930`，且 `minBarToDate` 能还原 `20330101`
- 实际行为: `(uDate - 19900000) * 10000` 全程在 `uint32_t` 里计算后才升宽，`430101 * 10000 = 4301010000` 溢出回绕成 `6042704`，返回 `6043634`；`minBarToDate` 还原出 `19900604`
- 复现步骤: 见用例
- 影响范围: 2032-10 之后的分钟线时间戳全部错乱（`uDate-19900000 > 429496` 即触发），影响 K 线缓存、回测索引
- 备注: 修复方向：`(uint64_t)(uDate - 19900000) * 10000`。不阻塞覆盖率

## BUG-L01-009

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Share/TimeUtils.hpp` `TimeUtils::Time32::fmt` (L423-L430)
- 相关测试: `l01_timeutils_test.cpp: time32_fmt_returns_shared_static_buffer`
- 输入/前置条件: 两个 `Time32` 实例在同一表达式里分别调用 `fmt()`，或不同线程并发调用
- 期望行为: 每次调用返回各自独立的缓冲（或 `std::string`）
- 实际行为: `static char buff[1024]` 是函数级静态（非 `thread_local`），所有实例/线程共用，前一次结果被覆盖；两个返回值指针相同
- 复现步骤: `EXPECT_EQ(a.fmt(), b.fmt())` 断言指针相等（当前实现）
- 影响范围: 日志格式化、行情时间戳拼接等并发路径存在数据竞争与串字风险
- 备注: 修复方向：改 `thread_local static` 或直接返回 `std::string`。不阻塞覆盖率

## BUG-L01-010

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Share/ObjectPool.hpp` `ObjectPool::destroy` (L70-L74)
- 相关测试: `l01_objectpool_test.cpp: DISABLED_destroy_null_is_undefined_and_rejected`
- 输入/前置条件: `pool.destroy(nullptr)`
- 期望行为: 判空后直接返回，或断言提示
- 实际行为: 无条件 `pobj->~T()`，对空指针解引用（UB）
- 复现步骤: 取消 `DISABLED_` 前缀运行会崩溃
- 影响范围: 调用方漏判空时进程崩溃；目前生产调用点大多做了判空
- 备注: 修复方向：入口加 `if (pobj == nullptr) return;`。不阻塞覆盖率

## BUG-L01-011

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/Share/Converter.hpp` `convert::from_str(T, std::string&)` (L44-L50)
- 相关测试: `l01_converter_charconv_test.cpp: from_str_string_overload_is_not_compilable`（改为只覆盖 `const char*` 重载）
- 输入/前置条件: 任何对该重载的实例化，如 `convert::from_str<int32_t>(someStdString)`
- 期望行为: 正常解析 `std::string`
- 实际行为: 编译期报错 `no matching function for call to 'from_chars'`（libc++：`std::string::iterator` 是 `__wrap_iter<char*>`，不能隐式转 `const char*`）。此外该重载里是 `T ret;` 未初始化（`const char*` 版本是 `T ret{}`），解析失败时 `noexcept` 吞错并返回不确定值
- 复现步骤: 在测试里写 `std::string s="1"; convert::from_str<int32_t>(s);` 后按语法门禁命令编译
- 影响范围: 该重载在所有平台上都不可用（未被生产代码实例化所以没暴露）；若有人调用会直接编译失败
- 备注: 修复方向：`std::from_chars(str.data(), str.data() + str.size(), ret)` 并把 `ret` 零初始化。为让测试 TU 通过语法门禁，本轮未编写调用该重载的用例（未放宽断言，只是无法编译）

## BUG-L01-012

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/Share/CodeHelper.hpp` `extractStdChnFutOptCode` (L547-L587)
- 相关测试: 未编写可执行用例（触发即越界写，会拖垮测试进程）；`l01_codehelper_test.cpp` 只用 ay[1] 含数字的期权代码
- 输入/前置条件: `extractStdChnFutOptCode("SHFE.ag.C.4000")` 之类交易所为 SHFE/INE/CZCE/其它、但 `ay[1]` 不含数字的代码
- 期望行为: 参数非法时返回空 `CodeInfo` 或抛异常
- 实际行为: `indexCodeMonth` 返回 `-1`，随后 `memcpy(codeInfo._product, ay[1].c_str(), mpos)` 以 `(size_t)-1` 为长度拷贝 → 越界读写/崩溃
- 复现步骤: 构造上述输入调用一次
- 影响范围: 期权代码解析入口 `extractStdCode` 也会转进来，脏数据可直接崩溃
- 备注: 修复方向：`mpos < 0` 时提前返回。不阻塞覆盖率

## BUG-L01-013

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/Share/CodeHelper.hpp` `extractStdCode` (L592-L661)
- 相关测试: 未编写可执行用例（同理会崩溃）
- 输入/前置条件: ①不含 `.` 的代码，如 `extractStdCode("600000", nullptr)`；②第三段为空的代码，如 `extractStdCode("SSE.STK.", nullptr)`
- 期望行为: 非法代码返回清零的 `CodeInfo`
- 实际行为: ①`StrUtil::findFirst` 返回 `npos` → `wt_strcpy(codeInfo._exchg, stdCode, npos)` 以 `npos` 为长度拷贝，并且 `stdCode + idx + 1` 越界；②`extlen == 0` 时读取 `ext[extlen - 1]` 即 `ext[-1]` 越界读
- 复现步骤: 分别调用上述两种输入
- 影响范围: 所有上游脏代码/异常代码输入的解析路径
- 备注: 修复方向：入口判 `idx == npos`、`extlen == 0`。不阻塞覆盖率

## BUG-L01-014

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Share/CodeHelper.hpp` `rawFutOptCodeToStdCode` (L443-L477)
- 相关测试: `l01_codehelper_test.cpp: raw_fut_opt_code_to_std_code_unmatched`（只用 `idx >= 3` 的安全输入）
- 输入/前置条件: ①全数字代码，如 `"123456"`；②非数字位出现在前 3 个字符内，如 `"P11600"`
- 期望行为: 参数非法时返回空串或抛明确异常
- 实际行为: ①`for (; idx >= 0; idx--)` 的 `idx` 是 `std::size_t`，全数字时下标回绕成 `SIZE_MAX` 继续越界读；②`append(code, idx - 3)` 与 `s.append(&code[idx - 3], 3)` 在 `idx < 3` 时下标回绕，读越界并可能抛 `std::out_of_range`
- 复现步骤: `CodeHelper::rawFutOptCodeToStdCode("123456", "CZCE")`
- 影响范围: 郑商所/上期所期权代码解析；脏数据可越界
- 备注: 修复方向：改有符号下标并校验 `idx >= 3`。不阻塞覆盖率

## BUG-L01-015

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Share/CodeHelper.hpp` `isMonthlyCode` (L374-L436)、`isStdChnFutOptCode` (L136-L228)
- 相关测试: `l01_codehelper_test.cpp: is_monthly_code_true / is_monthly_code_false`
- 输入/前置条件: 阅读状态机；例如 `isMonthlyCode("IF211")`
- 期望行为: 分支可达且字符集判定只接受字母
- 实际行为: ①`else if (4 == state)`（L403）不可达，前一个 `3 == state || 4 == state` 分支已吞掉 `state==4`，且该分支里数字会推进 state 而没有 `else`，`state` 停在 3/4 时不推进循环；②3 位数字尾巴也被判为月合约（`IF211` 返回 true），与注释“最后 3-6 位都是数字”一致但比直觉宽松；③`'A' <= ch && ch <= 'z'` 把 `[ \ ] ^ _ `` ` 6 个非字母字符也纳入字母集
- 复现步骤: `EXPECT_TRUE(CodeHelper::isMonthlyCode("IF211"))`
- 影响范围: 极端代码可能被误判为分月合约
- 备注: 非阻塞，测试已按实际行为断言并加注释

## BUG-L01-016

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Share/CodeHelper.hpp` `rawMonthCodeToStdCode` (L286-L336)、`rawFlatCodeToStdCode` (L342-L372)
- 相关测试: 未编写（避免依赖越界行为）
- 输入/前置条件: `rawMonthCodeToStdCode("cu01", "SHFE")`；或 `exchg`/`pid`/`code` 总长超过 64 字节
- 期望行为: 越界长度应报错或截断；返回值长度与可见文本一致
- 实际行为: 3 位分支走 `wt_strcpy(buffer + len, s, 3)` + `len += 3`，源串不足 3 位时返回串尾部带多余 `'\0'`（`"SHFE.cu.201\0"`，`size()==12`）；两个函数都用 `thread_local static char buffer[64]` 且无长度校验，超长输入直接 `memcpy` 越界
- 复现步骤: 传入 `exchg` 为 60 字符时必然越界
- 影响范围: 异常长代码（例如外部接口的完整符号名）会破坏栈/相邻缓冲
- 备注: 修复方向：长度校验 + 显式补 `'\0'`。不阻塞覆盖率

## BUG-L01-017

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Share/Converter.hpp` `convert::to_int32/to_uint32/to_int64/to_uint64` (L52-L70)
- 相关测试: `l01_converter_charconv_test.cpp: to_int_helpers_do_not_validate_digits`
- 输入/前置条件: `to_int32("-5")`、`to_int32("1a")`
- 期望行为: 非数字返回 0 或错误标志；负号得到负值
- 实际行为: 无 digit 校验、无符号处理，直接 `h * 10 + ch - '0'`：`to_int32("-5") == -25`，`to_int32("1a") == 59`；长串还会静默溢出
- 复现步骤: 见用例（已按实际行为断言）
- 影响范围: 用这些函数解析日期/时间数字串时，脏数据变成看似合理的错数字
- 备注: 修复方向：加 `isdigit` 校验并处理符号。不阻塞覆盖率

## BUG-L01-018

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Share/TimeUtils.hpp` `TimeUtils::getWeekDay` (L201-L221)
- 相关测试: 未编写（触发依赖极端日期/环境）
- 输入/前置条件: 超出 `time_t` 表示范围的 `uDate`（例如 `getWeekDay(99999999)`）
- 期望行为: 返回 0 或哨兵值
- 实际行为: `mktime`/`localtime` 返回 `NULL` 时直接 `tNow->tm_wday` 解引用空指针（同文件的 `isWeekends` 做了 `NULL` 检查，此处漏了）
- 复现步骤: `TimeUtils::getWeekDay(99999999)`
- 影响范围: 非法日期入参时崩溃
- 备注: 修复方向：与 `isWeekends` 一致地判空。不阻塞覆盖率

## BUG-L01-019

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Share/CodeHelper.hpp` `stdCodeToStdCommID2` (L242-L264)
- 相关测试: `l01_codehelper_test.cpp: std_code_to_std_comm_id`
- 输入/前置条件: `stdCodeToStdCommID2("SSE.600000")`
- 期望行为: 按函数头注释「如果是简化的股票代码，如 SSE.600000，则转成 SSE.STK」
- 实际行为: 两段代码原样返回 `"SSE.600000"`，没有任何股票/币种映射
- 复现步骤: `EXPECT_EQ(CodeHelper::stdCodeToStdCommID2("SSE.600000"), std::string("SSE.STK"))` 失败
- 影响范围: 文档与实现不一致，依赖注释的调用方会拿到错误品种 ID
- 备注: 修复方向：更新注释或补齐映射逻辑（需与 `IHotMgr`/`WtPktCodec` 口径一起评审）。测试按实际行为断言并加注释
