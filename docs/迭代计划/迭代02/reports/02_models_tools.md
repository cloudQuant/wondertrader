# 迭代 02：models/tools 候选逐条复核

日期：2026-10-29。范围：/tmp/wondertrader-review-20261029/groups/02_models_tools.json 中 29 条、6 个 lane。仅核对原始登记、生产实现、公开接口和关联测试定义；未改原 bug 记录、生产代码、测试、CMake 或 build。

## 裁定摘要

| 裁定 | 数量 | 说明 |
|---|---:|---|
| PRODUCT | 16 | 有公开 API、有效生命周期或已承诺错误处理语义支撑；本轮均为静态证据 |
| UNDECIDED | 12 | 实现差异或边界风险成立，但缺少允许输入、时间/金额语义或对象就绪条件 |
| NOT_BUG | 1 | ShareBlocks 两项越界主张都未由活动实现支持 |
| TEST | 0 | 没有候选能仅靠判定“产品主张不成立”结束 |
| DUPLICATE | 0 | 29 条中可保留 canonical 项；跨 lane 合并关系在对应条目中标明 |

triage/02_models_tools.json 含完整 29 条裁定，字段均按分组契约填写。没有本轮实际运行的复现，故 LOCAL_REPRO 为 0；静态确认项标 STATIC，缺业务/输入合同的项标 PENDING。

### R12_headers_wtstools

| 编号 | 裁定 | 复核结论 |
|---|---|---|
| BUG-R12-02 | PRODUCT | Queue 可存 NULL，clear/front/back 消费路径不统一判空。禁用测试尚未运行。 |
| BUG-R12-03 | PRODUCT | object/array 可由公开工厂创建，asBoolean 按 string union 成员读取；与 L02-001 同根并以本编号为 canonical。生产嵌套配置路径未证实，不扩大影响声明。 |
| BUG-R12-05 | PRODUCT | HashMap add 允许 NULL，grab/clear 解引用；当前测试仅测非空条目。 |
| BUG-R12-06 | PRODUCT | WTSTickSlice 空创建后的 _count 未初始化，且仓内存在创建/追加调用点；与 R1-001 合并。 |
| BUG-R12-07 | PRODUCT | 空 slice 的 at 返回 NULL，极值函数直接解引用；现有空 slice 测试没有检查极值。 |
| BUG-R12-10 | PRODUCT | iconv 失败哨兵比较错误；UTF8toChar 用剩余容量作终止符下标，零输出可写到分配区后一位。普通字符串断言不能证明无越界。 |
| BUG-R12-11 | UNDECIDED | ASCII A..z 包含标点，但 BTC_USDT 不属于该 helper 注释规定的分月期货输入；L01 还把下划线用作预期合法字符。缺字符规范。 |
| BUG-R12-12 | UNDECIDED | 32 字节 code 缓冲区的超长风险明确，合法最大长度未定。CodeHelper 尾随 NUL 是 L01-002 子项，R12 wrapper 仅比较 c_str，遗漏长度差异。 |

### c00_controller_adjudication

| 编号 | 裁定 | 复核结论 |
|---|---|---|
| BUG-C00-001 | PRODUCT | 完全不相交区间仍返回非空空数组，与函数注释的越界返回 NULL 相悖。历史记录说新增契约测试曾失败，但不是本轮重跑结果。原先“越界应夹到最后一根”的错误测试期望已修正。 |

### l02_wtstools_mq

| 编号 | 裁定 | 复核结论 |
|---|---|---|
| BUG-L02-005 | PRODUCT | Queue pop 删除槽位但未归还队列引用；现测试的正确引用计数断言保留，随后有兼容缺陷的条件补偿。 |
| BUG-L02-006 | PRODUCT | 四种容器的原子递减和零值读取分开，多引用并发 release 可重复清理；当前关联集合测试为单线程，尚无并发复现。 |
| BUG-L02-009 | UNDECIDED | ret*100 窄化 int32 的边界风险成立，但手续费金额、价格/数量范围及负值合同未知。 |
| BUG-L02-010 | PRODUCT | CSV reader 接受短行；check_cell 却按表头列数检查后访问当前行 vector。现有缺列测试只测未知字段/超表头索引。 |
| BUG-L02-011 | PRODUCT | fields() 函数静态缓存导致第二个 reader 返回第一个 reader 字段；已查到的生产用途是日志。空串 substr(0, size()-1) 的长度下溢为 npos，不构成 UB。 |
| BUG-L02-012 | NOT_BUG | BLK_FLAG 的 sizeof 大于 FLAG_SIZE，但活动代码无 sizeof 拷贝；字符串槽空间不足时 make_valid 检查后返回 NULL，不写越界。 |

### l03_includes

| 编号 | 裁定 | 复核结论 |
|---|---|---|
| BUG-L03-001 | PRODUCT | 两个公开 WTSHisTickData::create 重载都允许单字符串实参，形成编译歧义；现有调用显式传容量或 bool，不构成复现。 |
| BUG-L03-002 | PRODUCT | 部分公开 getter 读未初始化标量/枚举/指针/固定字符数组；测试 helper 先 set 后读，绕过默认创建状态。默认语义仍需定义。 |
| BUG-L03-003 | UNDECIDED | 2400/0 取模别名是事实，接口是否应允许混叠待时间合同。给定单段案例不能证明 isInTradingTime(0) 错误。 |
| BUG-L03-004 | PRODUCT | setter 改 offset 不重算 sections，也不清 _minutes；当前测试断言并固定了对象不自洽状态。 |
| BUG-L03-005 | UNDECIDED | 缺 commodity 时确有条件性空指针路径，但 setCommInfo 是否为调用前置条件未写明；disabled 测试不是运行证据。 |
| BUG-L03-006 | UNDECIDED | 无容量 wt_strcpy 确有超长写风险；有效外部字符串长度合同缺失，现有用例只测 63 字符写入 64 字节数组。 |
| BUG-L03-007 | UNDECIDED | minuteToTime 钳到 close、secondsToTime 返回 sentinel 的差异被现测试明确固定；未找到要求一致策略的 API 合同。 |
| BUG-L03-008 | UNDECIDED | 竞价映射为分钟 0，谓词随之可能返回 true；测试只断言映射，未规定竞价是否属于 trading time。 |
| BUG-L03-009 | UNDECIDED | 任意偏移/时段值能令所谓死分支可达的静态算术成立；但该条推翻“分支不可达”论断，不是有效会话域内已证产品缺陷或覆盖率证据。 |
| BUG-L03-010 | UNDECIDED | 零总量 fallback 和 section list 口径不同；900–900 是否表示全天、零时长或非法配置尚无合同，现测试也未给出一致 oracle。 |

### p12_datafactory_datadef

| 编号 | 裁定 | 复核结论 |
|---|---|---|
| BUG-P12-002 | UNDECIDED | 秒周期为零时的除法点真实存在；周期是否必须为正尚无 API 合同，禁用的宽泛测试未运行。 |
| BUG-P12-003 | PRODUCT | WTSValueArray/WTSKlineData 的注释承诺越界返回 sentinel，但 end==size 被放行并由包含式循环访问越界。禁用测试只触碰 WTSValueArray maxvalue，本轮未跑 ASan。WTSKlineSlice 空极值另由 R12-07 追踪。 |
| BUG-P12-005 | UNDECIDED | extractDayData 对 add 赋值与其他聚合路径不同，但跨日 add 口径未定。当前 DayResampleGroupsByCount 已撤掉 add 固定值断言。 |
| BUG-P12-006 | PRODUCT | 新日/新秒桶返回 detached bar；WtDataManager 忽略 updateKlineData 返回值，随后从旧 kData 取末 bar 回调，存在新 bar 未入缓存/回调错误及 detached 对象泄漏。已有测试只覆盖 helper。 |

## 测试错误与误绿风险

1. src/TestUnits/coverage/l03_includes/l03_session_info_test.cpp:483-495 的 set_offset_mins_leaves_sections_stale 把缺陷行为当成预期：断言 open/close 仍为旧 offset 值，同时断言新 offsetTime 不同。产品项仍保留；修复后该测试应改为断言新旧换算自洽，并覆盖 trading、auction 和已缓存的分钟列表。
2. src/TestUnits/coverage/r12_headers/r12_charconv_safe_test.cpp:38-50 把“无效编码得到空串”当作安全结果，但 src/Share/charconv.hpp:119-127 在零字节输出时可能把 NUL 写到缓冲区后一字节。字符串断言可以通过而不触发检测；应先修正容量/剩余容量计算，再用 ASan 验证。
3. src/TestUnits/coverage/r12_headers/r12_codehelper_test.cpp:183-187 对返回值只做 c_str 文本比较，并注释称尾部 NUL 是预期填充；这掩盖 std::string size 多 1 的 BUG-L01-002。L01 当前已有完整 string/size 断言，R12 wrapper 应同步改正。
4. src/TestUnits/coverage/l03_includes/l03_session_info_test.cpp:435-445、:398-409、越界反查测试以及 P12-005 的现行测试，均是对当前实现行为的断言，但各自缺少产品合同 oracle；不能仅因期望与候选报告冲突就改成相反断言。

## 可运行入口与命令边界

本轮没有运行测试或构建。CMake 把 coverage 下未分派的测试源加入 CoverageTests 主目标：src/TestUnits/coverage/CMakeLists.txt:18-23,448-475；现有可执行文件为 build-cov/build_x64/Debug/bin/TestUnits/CoverageTests。上述六个 lane 没有单独的 lane 二进制。已存在的二进制不证明包含当前 dirty 测试源码，修改测试后需按主控流程定向重建。

可用于修复后的定向命令示例（每条使用独立 profile 路径，避免写到默认工作目录）：

    LLVM_PROFILE_FILE=/tmp/wt-l03-004-%p.profraw build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_filter='l03_session.set_offset_mins_leaves_sections_stale'
    LLVM_PROFILE_FILE=/tmp/wt-r12-10-%p.profraw build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_filter='r12_charconv_utf8tochar.unconvertible_input_yields_empty_buffer'
    LLVM_PROFILE_FILE=/tmp/wt-c00-001-%p.profraw build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_filter='ws03_kline_slice.extract_data_rejects_range_after_all_bars'
    LLVM_PROFILE_FILE=/tmp/wt-l02-005-%p.profraw build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_filter='L02VariantFixture.WtsQueuePushPopFrontBack'

不要直接用 --gtest_also_run_disabled_tests 执行会崩溃的旧禁用用例；修复后应拆成明确契约用例再启用。未执行这些命令；也未运行全量测试、Ninja dry run、构建或 CMake。

## 环境与证据限制

- 本次为静态只读复核，没有本轮 LOCAL_REPRO；上一轮历史报告中的 PASS/FAIL 仅作为历史记录，不折算成本次执行。
- 初始工作树存在大量未提交 bug 文档、测试及其他源码改动；本次没有覆盖这些路径。唯一新增文件是本报告与对应 triage JSON。
- triage/02_models_tools.json 已用 jq 检查：29 条、字段齐全、枚举有效；统计结果 PRODUCT 16、UNDECIDED 12、NOT_BUG 1。
