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
- 备注: 旧测试曾断言当前错误长度；现已改为期望完整字符串和可见长度，三条契约回归重建后 FAIL。修复应按可见字符数确定长度。

## BUG-L01-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Share/Converter.hpp` `convert::to_str` (L21-L34)
- 相关测试: `l01_converter_charconv_test.cpp: to_str_into_caller_buffer_terminates_each_result`、`to_str_internal_static_buffer_terminates_each_result`，以及零填充的 `to_str_into_caller_buffer` 对照。
- 输入/前置条件: 先 `to_str(buf, 32, 123456)` 再 `to_str(buf, 32, 7)`；或先 `to_str<int64_t>(9876543210LL)` 再 `to_str<int32_t>(1)`
- 期望行为: 第二次调用返回 `"7"` / `"1"`
- 实际行为: `std::to_chars` 不写结束符，函数又不清零缓冲区，短数字覆盖后残留上一次长数字的尾巴，返回 `"723456"`；单参版本共用 `thread_local` 缓冲区，跨用例顺序相关
- 复现步骤: 见上述用例
- 影响范围: 所有使用 `convert::to_str` 生成字符串的代码（日志、代码拼接、行情字段格式化）；短数值跟在长数值后面时输出错误数据
- 备注: 修复方向：用 `to_chars` 返回的 `ptr` 写 `*ptr = '\0'`（或返回 `to_chars_result`）。不阻塞覆盖率

## BUG-L01-004

- 状态: 已撤回（当前 API 文档明确安全字符保持原样；严格 URL 编码尚未形成已承诺契约）
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
- 相关测试: `l01_timeutils_test.cpp: get_next_month_normalizes_negative_months`；正确日历偏移断言已启用。
- 输入/前置条件: `getNextMonth(202401, -1)`、`getNextMonth(202406, -1)`
- 期望行为: `202312`、`202405`
- 实际行为: `uAddYear = months/12` 对负数截断为 `0`，`uAddMon = months%12` 再 `+12` 变成 `11`，年份不回退，结果 `202412`、`202505`（大一年）
- 复现步骤: 见用例
- 影响范围: 所有做“上月/上上月”推算的月份合约、展期逻辑
- 备注: 修复方向：把月份换算成绝对月序号 `y*12+m + months` 再还原。不阻塞覆盖率

## BUG-L01-008

- 状态: 已合并（BUG-R32-003）
- 严重级别: Blocker
- 模块/文件: `src/Share/TimeUtils.hpp` `TimeUtils::timeToMinBar` (L337-L340)
- 相关测试: `l01_timeutils_test.cpp: time_to_min_bar_roundtrips_historical_and_future_dates`；已将旧的溢出结果断言与禁用的正确往返用例合并。
- 输入/前置条件: `timeToMinBar(20330101, 930)`
- 期望行为: `4301010930`，且 `minBarToDate` 能还原 `20330101`
- 实际行为: `(uDate - 19900000) * 10000` 全程在 `uint32_t` 里计算后才升宽，`430101 * 10000 = 4301010000` 溢出回绕成 `6042704`，返回 `6043634`；`minBarToDate` 还原出 `19900604`
- 复现步骤: 见用例
- 影响范围: 2032-10 之后的分钟线时间戳全部错乱（`uDate-19900000 > 429496` 即触发），影响 K 线缓存、回测索引
- 备注: 修复方向：`(uint64_t)(uDate - 19900000) * 10000`。不阻塞覆盖率

## BUG-L01-009

- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
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

- 状态: 已撤回（destroy 接受空指针的契约未确立）
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

- 迭代02第二轮前状态（历史）: - 状态: 公开 helper 非法输入的静态边界风险；生产调用可达性未证实
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定
- 模块/文件: `src/Share/CodeHelper.hpp` `extractStdChnFutOptCode` (L547-L587)
- 相关测试: 未编写可执行用例（触发即越界写，会拖垮测试进程）；`l01_codehelper_test.cpp` 只用 ay[1] 含数字的期权代码
- 输入/前置条件: `extractStdChnFutOptCode("SHFE.ag.C.4000")` 之类交易所为 SHFE/INE/CZCE/其它、但 `ay[1]` 不含数字的代码
- 期望行为: 参数非法时返回空 `CodeInfo` 或抛异常
- 实际行为: `indexCodeMonth` 返回 `-1`，随后 `memcpy(codeInfo._product, ay[1].c_str(), mpos)` 以 `(size_t)-1` 为长度拷贝 → 越界读写/崩溃
- 复现步骤: 直接调用 helper 的畸形输入路径；本轮未运行。`extractStdCode` 先校验格式，该例子不能作为经由此入口到达 helper 的复现。
- 影响范围: 直接调用 helper 且绕过格式校验时有越界风险；仓内生产调用绕过校验的证据未发现。
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
- 复核（2026-10-29）: 原 `r7_execrunner.queries_return_null_before_base_data_loaded` 把无点字符串 `nosuchcode` 送入标准代码查询，实测进程异常退出；定位到这里的 `npos` 拷贝。原测试的空基础数据场景已改用合法代码，该结果不证明第二种空末段输入已运行复现。
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

- 状态: 已合并（字符范围候选归入 BUG-R12-11；死分支和三位尾数不构成已证产品缺陷）
- 严重级别: Minor
- 模块/文件: `src/Share/CodeHelper.hpp` `isMonthlyCode` (L374-L436)、`isStdChnFutOptCode` (L136-L228)
- 相关测试: `l01_codehelper_test.cpp: is_monthly_code_true / is_monthly_code_false`
- 输入/前置条件: 阅读状态机；例如 `isMonthlyCode("IF211")`
- 期望行为: 分支可达且字符集判定只接受字母
- 实际行为: ①`else if (4 == state)`（L403）不可达，前一个 `3 == state || 4 == state` 分支已吞掉 `state==4`；循环下标仍递增，不能说循环不推进；②3 位数字尾巴也被判为月合约（`IF211` 返回 true），与注释“最后 3-6 位都是数字”一致；③`'A' <= ch && ch <= 'z'` 把 `[ \ ] ^ _ `` ` 6 个标点字符也纳入该范围，有效字符合同待确认，归入 BUG-R12-11。
- 复现步骤: `EXPECT_TRUE(CodeHelper::isMonthlyCode("IF211"))`
- 影响范围: 极端代码可能被误判为分月合约
- 备注: 非阻塞，测试已按实际行为断言并加注释

## BUG-L01-016

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/Share/CodeHelper.hpp` `rawMonthCodeToStdCode` (L286-L336)、`rawFlatCodeToStdCode` (L342-L372)
- 相关测试: 未编写（避免依赖越界行为）
- 输入/前置条件: `rawMonthCodeToStdCode("ag", "SHFE", true)` 等品种输入；或 `exchg`/`pid`/`code` 总长超过 64 字节
- 期望行为: 越界长度应报错或截断；返回值长度与可见文本一致
- 实际行为: `isComm` 分支将终止 NUL 计入 `std::string` 长度，合法品种输入的完整字符串断言重建后 FAIL；两个函数都用 `thread_local static char buffer[64]` 且无长度校验，超长输入可能越界。旧 `cu01` 是两位月码尾数，是否受支持未证明，不再作为确定产品缺陷证据。
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

- 状态: 条件性防御风险；可达输入和动态复现未确认
- 严重级别: Minor
- 模块/文件: `src/Share/TimeUtils.hpp` `TimeUtils::getWeekDay` (L201-L221)
- 相关测试: 未编写（触发依赖极端日期/环境）
- 输入/前置条件: `localtime` 返回 NULL；尚未确认某个 `uDate` 在目标平台上能触发该条件。
- 期望行为: 返回 0 或哨兵值
- 实际行为: `mktime`/`localtime` 返回 `NULL` 时直接 `tNow->tm_wday` 解引用空指针（同文件的 `isWeekends` 做了 `NULL` 检查，此处漏了）
- 复现步骤: 待找到目标平台的有效触发输入；`99999999` 可能被 `mktime` 归一化，不能作为已证崩溃复现。
- 影响范围: 若 `localtime` 返回 NULL，解引用会产生未定义行为；合法日期路径是否可达未证实。
- 备注: 修复方向：与 `isWeekends` 一致地判空。不阻塞覆盖率

## BUG-L01-019

- 迭代02第二轮前状态（历史）: - 状态: 注释与实现冲突；股票简码映射合同待确认
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: Minor
- 模块/文件: `src/Share/CodeHelper.hpp` `stdCodeToStdCommID2` (L242-L264)
- 相关测试: `l01_codehelper_test.cpp: std_code_to_std_comm_id`
- 输入/前置条件: `stdCodeToStdCommID2("SSE.600000")`
- 期望行为: 按函数头注释「如果是简化的股票代码，如 SSE.600000，则转成 SSE.STK」
- 实际行为: 两段代码原样返回 `"SSE.600000"`，没有任何股票/币种映射
- 复现步骤: `EXPECT_EQ(CodeHelper::stdCodeToStdCommID2("SSE.600000"), std::string("SSE.STK"))` 失败
- 影响范围: 文档与实现不一致，依赖注释的调用方会拿到错误品种 ID
- 备注: 修复方向：更新注释或补齐映射逻辑（需与 `IHotMgr`/`WtPktCodec` 口径一起评审）。测试按实际行为断言并加注释

## 独立逐项复核（2026-10-29）

本节只读核对源码、接口形状、仓库内调用点和测试定义；本次没有运行测试或编译触发用例。测试中的断言、`DISABLED_` 用例和本清单中的既有“实测”记录不作为本次动态复现证据。BUG-L01-013 第 174 行的既有复现记录原样保留；本次不重跑、不覆盖其历史记录，并将第二种空末段输入标为尚未单独复现。

### BUG-L01-001：源码缺陷确认；与 BUG-R12-01 重复

`CodeHelper::find`（`src/Share/CodeHelper.hpp:104-129`）反向循环的 `size_t idx >= 0` 恒真；无匹配时下标回绕，越过源串边界。`stdCodeToStdHotCode/stdCodeToStd2ndCode` 后面的 `npos` 返回分支表明包装函数本来希望对无点输入安全返回，故这里不是单纯对合法标准码的额外要求。L01 没有可执行的无点用例，现有 `l01_codehelper_test.cpp` 主要使用带分隔符的代码；R12 的对应测试为禁用用例，本次均未运行。建议改成有界反向扫描（找到后返回索引，扫完返回 `npos`），不要用无符号 `idx >= 0`。

### BUG-L01-002：返回字符串长度缺陷确认；作为 BUG-R12-12 的 CodeHelper 子项主记录，错误测试期望已纠正

`stdCodeToStdHotCode/stdCodeToStd2ndCode`（`src/Share/CodeHelper.hpp:482-508`）的 `resize` 长度多加 1，而 `wt_strcpy` 已写入终止符，返回的 `std::string` 因而包含尾部 NUL。旧 L01 测试固化了错误长度；现已改为检查完整字符串相等和可见长度，重建后 3 条契约用例 FAIL。建议长度设为可见前缀与后缀之和。

### BUG-L01-003：C 字符串终止缺陷确认；错误测试期望已纠正

`convert::to_str`（`src/Share/Converter.hpp:21-34`）不处理 `std::to_chars` 返回的 `ptr`，而 `to_chars` 不写 NUL。复用旧缓冲时短结果后仍有旧尾巴；静态缓冲和调用方缓冲都受影响。旧测试把残留尾巴作为预期；现已用分配内的 canary 和有界扫描验证两种重载的终止符，重建后两例 FAIL，另一个零填充控制 PASS。建议成功转换后用返回的 `ptr` 写 NUL，并检查 `ec` 和可用容量。

### BUG-L01-004：按当前 API 文档不是产品缺陷；属于待定的严格 URL 编码需求

`URLEncode` 的类注释（`src/Share/charconv.hpp:396-400`）明确写着空格转 `%20`、其余 ASCII 保持不变；`URLDecode` 的注释（`:477-482`）也明确约定安全字符即使以 `%XX` 表示仍保持原样。实现（`:403-427,486-526`）和现有测试（`l01_converter_charconv_test.cpp:171-195`）与该约定一致，因此 `%41` 不解成 `A`、`%` 不转 `%25` 本身不能判为实现偏离。若调用方需要完整 URL 组件百分号编码，应先定义组件/表单语义和兼容方式，再新增严格模式与往返测试；不要把当前文档未承诺的 RFC 行为当成已证缺陷。

### BUG-L01-005：源码缺陷确认；历史挂死记录保留但本次未复现

`EncodingHelper::isGBK`（`src/Share/charconv.hpp:560-581`）遇到高位字节但双字节条件失败时既不返回也不推进 `i`，循环会卡在该字节；高位字节位于末尾时还会先读 `data[i+1]`。这些路径违反函数按长度读取数据的接口形状。原条目第 65 行记录了一次 9 分 28 秒的历史超时；对应坏输入用例现为 `DISABLED_`（`l01_converter_charconv_test.cpp:260-275`），本次没有重跑。该问题已在 R16 的避重说明（`R16_share_tools.md:6`）中作为既有项引用，不应重复计数。建议先检查剩余长度，再明确失败字节的推进/返回规则。

### BUG-L01-006：大偏移量归一化缺陷确认；生产调用边界尚未证明会传入大于一天的偏移

`TimeUtils::getNextMinute`（`src/Share/TimeUtils.hpp:299-312`）只加/减一次 1440；对 `mins=-1500` 加一天后仍为负，最终转成 `uint32_t` 得到大数。旧测试固化了错误值；现已启用日内归一化期望，重建后单例 FAIL：预期 23:00、实际 `4294967196`，小偏移及跨午夜控制 PASS。生产调用目前只见 `-1` 或 `+1` 等小偏移，生产触发频率未证实。建议按 `totalMins` 做完整的日内模运算。

### BUG-L01-007：负月份偏移缺陷确认；生产调用点未找到

`getNextMonth`（`src/Share/TimeUtils.hpp:319-334`）对负数使用向零截断的除法/余数，再把负余数加 12，却没有同步回退年份；例如 202401 加 -1 会算到 202412。旧测试将错误结果设为启用期望；现改为正确的负偏移回归，本机 FAIL，正向与零偏移对照 PASS。源码检索未找到生产调用方，因此确认的是工具函数的输入/输出错误，线上影响尚无调用证据。建议用绝对月份序号计算并还原年月，覆盖年界两侧的正负偏移。

### BUG-L01-008：有效日期上的整数溢出缺陷确认；与 BUG-R32-003 重复，原“2032-10”起点不准确

`timeToMinBar`（`src/Share/TimeUtils.hpp:337-340`）先以 `uint32_t` 计算 `(uDate-19900000)*10000`，再提升到 `uint64_t`；20330101 已产生回绕，且该 helper 被 WTSDataFactory、WtDataWriter 等生产路径调用。该项与 `docs/迭代计划/迭代01/bugs/R32_adstorage.md:60-78` 的 BUG-R32-003 是同一缺陷。有效 `YYYYMMDD` 日期的首个年度越界样例是 2033 年，而不是 2032 年 10 月；原测试固定溢出结果，现合并成启用的正确往返用例。独立测试 FAIL，20330101 返回日期 19900604，历史日期对照 PASS。建议在乘法前将日期差提升到 `uint64_t`，并以 2033 有效日期验收。

### BUG-L01-009：共享缓冲行为确认；是否违反 API 契约待定，未发现生产 `fmt()` 调用

`Time32::fmt`（`src/Share/TimeUtils.hpp:423-429`）确实返回同一函数级静态缓冲；再次调用会覆盖前值，并发调用也会共享写入。测试 `l01_timeutils_test.cpp:257-269` 只确认指针相同，没有验证文档/API 是否承诺返回独立且可保留的结果。源码检索没有发现生产 `.fmt()` 调用点，因此不能据此确认当前业务影响。建议明确返回值有效期和线程安全契约；若调用方需保留多个结果，返回 `std::string` 或接收调用方缓冲。仅改成 `thread_local` 只能隔离线程，不能解决同线程连续调用后旧指针内容被覆盖。

### BUG-L01-010：NULL 输入目前不构成已确认的产品缺陷

`ObjectPool::destroy`（`src/Share/ObjectPool.hpp:63-74`）要求销毁一个对象，并直接调用析构函数和归还池内存；源码/注释没有承诺 `destroy(nullptr)` 是合法操作。构造失败可返回 NULL（`:53-60`），但调用方是否必须只把 `construct()` 成功返回的同池对象传给 `destroy`，应写成明确前置条件。禁用测试 `l01_objectpool_test.cpp:228-235` 的“NULL 应拒绝”是额外健壮性要求，不是已证接口保证。本次未运行。建议先定契约：非法参数应由调用方检查并文档化；如需容忍 NULL，再加 guard 和对应测试。

### BUG-L01-011：存在可确认的泛型重载问题；编译失败只在特定标准库下有现有记录，本次未编译

`convert::from_str<T>(std::string&)`（`src/Share/Converter.hpp:44-50`）把 `std::string::iterator` 传给 C++17 `std::from_chars` 的字符指针范围接口；libc++ 的迭代器不是 `const char*`，因此该重载不能在那里实例化。另有 `T ret;` 未初始化且忽略错误结果：无效输入时 `from_chars` 不写入 value，随后返回未初始化值。对应测试 `l01_converter_charconv_test.cpp:91-106` 虽名为 `from_str_string_overload_is_not_compilable`，实际只调用 `const char*` 重载，不是编译复现；本次未运行编译。C++ 工作草案的 `from_chars` 字符串接口定义为 `const char*` 范围（[WG21 N4950 §28.2.3](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/n4950.pdf)）。建议用 `str.data()` 和 `str.data()+str.size()`，初始化结果，并定义/检查错误码和是否全量消费。

### BUG-L01-012：报告的 `extractStdCode` 生产入口不可达；直接 helper 的非法输入边界与 R55 已有记录重叠

`extractStdCode`（`src/Share/CodeHelper.hpp:592-598`）先调用 `isStdChnFutOptCode`，只有格式校验通过才进入 `extractStdChnFutOptCode`；该校验要求期权月份段含数字。因此例子 `SHFE.ag.C.4000` 会在识别阶段被拒绝，不会沿所述生产路径让 `indexCodeMonth` 返回 -1。`extractStdChnFutOptCode` 本身是可直接调用的方法，直接给不符合期权格式的段仍可能把 `mpos=-1` 转成超大长度传给 `memcpy`（`:569-583`）；这是非法参数边界，未找到生产调用直接绕过入口校验的证据。该负长度问题已被 `R55.md:5-6` 作为 L01 已记录项引用，不应重复计数。建议保留 `mpos < 0` 和分段数量校验，并考虑将仅供内部使用的 helper 收为私有。

### BUG-L01-013：无分隔符路径的越界风险确认；保留原有 R7 复现记录，空末段仅源码可疑

`extractStdCode`（`src/Share/CodeHelper.hpp:610-616`）未检查 `findFirst` 的 `npos` 就传给 `wt_strcpy` 并构造后续指针；`wt_strcpy` 对非零长度直接 `memcpy`（`src/Includes/WTSMarcos.h:98-103`），故没有点号的输入会形成非法长度/地址。原条目第 174 行记录的 R7 进程异常退出及定位保留原样，作为历史复现证据；本次未再次运行。第三段为空时 `extlen-1` 的下溢可从 `:626-634` 静态确认，但未看到独立运行复现；该表达式物理上可能落在分隔符前一字节，不能无证据断言必然段错误。建议验证分段数和每段非空后再取末字符，并保留原 R7 记录。

### BUG-L01-014：仅确认对格式非法输入的未校验风险；合法回退格式有覆盖，生产触发待验证

`rawFutOptCodeToStdCode`（`src/Share/CodeHelper.hpp:443-475`）在首个正则不匹配时不判断是否为有效的郑商所/上期所格式，就从尾部扫描并使用 `idx-3`。全数字输入会让无符号下标回绕；`P11600` 等短前缀会让长度/指针偏移越界。现有测试 `l01_codehelper_test.cpp:231-236` 只检查合法回退格式，没有覆盖这些非法形状；R12 对同一回退循环也有提及，但该输入不是有效期权代码。源码风险可确认，候选输入的生产来源/接受契约未证实，不能作为合法合约码下的已复现故障。建议正则不匹配后执行完整格式校验，校验最小长度和 `idx >= 3`，失败返回错误值。

### BUG-L01-015：死分支不是已证行为缺陷；3 位尾数符合注释，测试本身有矛盾，字符集需待定

`isMonthlyCode`（`src/Share/CodeHelper.hpp:374-435`）里 `else if (4 == state)` 被前面的 `state==3 || state==4` 覆盖，属于不可达代码；但循环的 `i` 仍持续递增，不是死循环。注释写“最后 3-6 位都是数字”，故 `IF211` 返回 true 与注释一致，不能据此认定 3 位尾数是产品缺陷。旧版测试曾对 `IF211` 同时断言 true 和 false；当前已保留 true，并把负例更换为以非数字结尾的 `IF21A`。`A-z` 字符范围候选与 BUG-R12-11 同一位置、同一疑问，归入该条；L01 的 `IF_o1` 下划线期望说明缺少字符规范时不能简单判定下划线非法。候选仍需明确有效品种字符集。

### BUG-L01-016：品种转码含尾部 NUL 已本机复现；64 字节缓冲溢出属于未定长输入风险

原 `cu01` 测试只含两位数字尾码，与仓库 3–6 位月码形状不符；已改为三位 `cu001`，单例 PASS。该事实不能证明公开 helper 必须支持两位尾码。另对 `isComm=true` 改用符合品种输入的 `ag/IF`，完整字符串断言单例 FAIL：返回值含多余尾部 NUL；源码在拷贝终止符后仍将其计入 `std::string` 长度。建议明确输入格式，并使返回字符串长度不包含终止符。

同条关于 `rawMonthCodeToStdCode/rawFlatCodeToStdCode` 的 64 字节 `thread_local` 缓冲（`:288-335,342-371`）确实没有总长度检查；超过容量会写越界，但未找到合法交易所、品种、合约字段组合可超过 63 字节的证据。`rawMonthCodeToStdCode` 返回时会立即复制到 `std::string`，不是把静态缓冲指针交给调用方。建议明确所有输入字段上限或改为动态字符串拼接；在验证有效长度上限前，将长输入部分列为边界风险，不与已复现的 `isComm` 长度问题混为一谈。

### BUG-L01-017：负数解析缺陷确认；非数字和溢出处理契约待补，当前测试锁定错误结果

`convert::to_int32`（`src/Share/Converter.hpp:52-55`）把每个字符都按 `ch-'0'` 累积，没有负号分支；合法负整数 `"-5"` 因而不能解析为 -5。该函数被 `CsvReader::get_int32` 直接调用（`src/WTSTools/CsvHelper.cpp:77-83`），所以负数 CSV 字段也受影响。旧测试固化 `-25` 和 `"1a" == 59`；现已改为合法负数应为 -5，并移除未约定非法输入结果的断言。重建后负数回归 FAIL（实际 -25），正数控制 PASS。非法输入和溢出如何处理仍需定义；建议使用有符号数字解析并明确错误与溢出行为。

### BUG-L01-018：空指针风险缺少可达复现；`99999999` 例子不足以确认故障

`getWeekDay`（`src/Share/TimeUtils.hpp:201-220`）没有检查 `localtime` 返回值，若它返回 NULL 会被解引用；但所列 `99999999` 会被拆成年/月/日后交给 `mktime` 归一化，或在不可表示时得到 `(time_t)-1`，单凭该值不能推出随后 `localtime` 必返 NULL。当前测试只覆盖常规日期（`l01_timeutils_test.cpp:170-196`），本次没有极端日期或平台复现，也未证明有效输入会触发失败。建议将此项列为防御性检查/平台待验证：显式处理 `mktime` 和 `localtime` 失败并先验证日历日期；取得目标平台可复现输入前不计确认崩溃缺陷。

### BUG-L01-019：源码注释与两段码实现不一致；是否改代码待契约确认

`stdCodeToStdCommID2` 注释（`src/Share/CodeHelper.hpp:242-246`）称 `SSE.600000` 要转成 `SSE.STK`，但实现（`:247-263`）对只有两个段的输入统一原样返回。现有测试只断言 `SSE.STK` 原样返回（`l01_codehelper_test.cpp:147-154`），没有测试注释举出的 `SSE.600000`；源码检索也没有找到生产调用者。可确认的是文档/实现契约冲突，不能仅凭注释认定当前通用两段码返回错误。建议由接口所有者决定注释过时还是需要对简化股票码加专门映射，再同步实现与精确测试。

另一个四段期权输入 `CFFEX.IO2008.C.4300` 的测试期望前两段，当前返回 `CFFEX.IO2008.C`；公开注释只明确了普通三段例子，四段期权品种 ID 的契约尚未确认，该失败保留待裁定，不据此认定产品缺陷。
