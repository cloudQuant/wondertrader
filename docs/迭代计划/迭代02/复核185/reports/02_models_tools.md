# 02_models_tools：185 条问题复核

复核范围为输入组 02 的 12 条。源码和测试只读；本组只保存了报告、triage 和本组 evidence。未构建、未运行全量测试，也未改 C++/测试/CMake。

## 结果

| 裁定 | 数量 | 说明 |
|---|---:|---|
| PRODUCT | 6 | tick code 固定缓冲、手续费窄化、配置字符串越界、session offset 溢出、相同端点视图不一致、零周期除零 |
| NOT_BUG | 5 | 代码格式候选无有效错误输入；ContractInfo 未挂接 builder 用法；午夜别名、反查边界差异、竞价归零都与现有契约/正常调用一致 |
| NEEDS_DECISION | 1 | 日线 add/ask 共用 union，通用重采样缺少字段语义输入 |
| DUPLICATE / TEST / EXTERNAL_BLOCKED / LOCAL_INCOMPLETE | 0 | 没有把独立 tick 缓冲问题误并入 L01-002 |

证据等级为 10 STATIC、1 LOCAL_REPRO、1 PENDING。LOCAL_REPRO 只有 BUG-L03-010；P12-005 仍待明确 add/ask 的分类型合同。本轮没有运行可用于裁定该语义的测试。

## 逐条复核

### R12_headers_wtstools

| 编号 | 裁定 | 结论 |
|---|---|---|
| BUG-R12-11 | NOT_BUG | A..z 包含标点是静态事实，但 BTC_USDT 不是注释所限定的分月期货输入，尾部也不满足 isMonthlyCode 的数字月码要求。现有 IF_o1 用例要求下划线前缀保留；没有有效输入的错误解析证据。 |
| BUG-R12-12 | PRODUCT | WTSTickData::create/setCode 对 32 字节固定 code 数组无长度校验，写入长度等于/大于 32 时越界。当前短码测试没有边界覆盖。此项与 CodeHelper 的尾随 NUL 不重复：R12 wrapper 现在使用完整 std::string 和 size 断言；CodeHelper 错误由 L01-002 单独跟踪。 |

R12-12 的修复要拒绝无法表示的字符串，不静默截断，以免把代码标识改成另一个合约。验收覆盖 0/1/31 字节和 32 字节以上，在 ASan 下分别走 create 与 setCode。

### l02_wtstools_mq

| 编号 | 裁定 | 结论 |
|---|---|---|
| BUG-L02-009 | PRODUCT | commodity 与 contract 的 calcFee 都把 ret*100 转成 int32。商品手续费配置为外部 JSON；price=5000、qty=1,000,000、volScale=300、by-value rate=0.00002 时，ret=30,000,000，ret*100=3,000,000,000，超过 int32 上限。这个有限正值不依赖负手续费合同。现有例子只测普通金额。 |

产品修复应改为不窄化到 int32 的安全舍入，并定义非有限乘积如何受控处理；不用负费率推断扩展合同。建议新增超过 int32 分值边界的有限正手续费回归。

### l03_includes

| 编号 | 裁定 | 结论 |
|---|---|---|
| BUG-L03-003 | NOT_BUG | section 以 2400 收盘时，0 点作为其午夜别名与左/右对齐规则一致。给定单段 2100–2400 的输入在 timeToMinutes(0) 会先成为 INVALID_UINT32，不能据此证明交易时间谓词出错。 |
| BUG-L03-005 | NOT_BUG | 保证金/部分手续费路径确实依赖 m_commInfo，但唯一生产构造路径先创建、挂接 commodity，再将 contract 放入已发布 code list。disabled 测试在未就绪 builder 上调用 getter，并期望无合同支持的 fallback 值；没有正常生产调用证据。建议修正该测试注释，不把危险路径启用成行为验收。 |
| BUG-L03-006 | PRODUCT | wt_strcpy 只按源长度复制并写 NUL。Commodity/Contract 固定字符字段由外部 JSON 直接填充，loader 没有长度验证；63 字符测试只证明 64 字节数组的安全上限，没有覆盖超限拒绝。 |
| BUG-L03-007 | NOT_BUG | minuteToTime 越界钳到 closeTime、secondsToTime 越界返回 INVALID_UINT32 的差异被测试明确固定。历史回放也会先将分钟位置钳到总时长；没有合同要求两个反向转换函数对非法越界值返回相同结果。 |
| BUG-L03-008 | NOT_BUG | session JSON 配置的竞价 tick 被映射到分钟/秒 0；注释明确如此。updateKlineData 以 isInTradingTime 判定是否纳入 tick，所以将竞价数据并入开盘 bucket 与数据用途一致。 |
| BUG-L03-009 | PRODUCT | 外部 session JSON 的 int32 offset 未验证就传到 offsetTime。有效 09:00 时段与 INT32_MAX offset 相加会溢出 int32；这证明的是算术安全缺陷，不代表极端 offset 是有效时区。用 int64 计算并按既有时间回绕规则归一化，再在配置边界执行时区域检查。 |
| BUG-L03-010 | PRODUCT | 900–900 配置被 loadSessions 接受。同一 session 的 getTradingMins/Seconds 回退为全天，而 getSecMinList 返回 [0]；ticker 和 replay 消费这两种视图。推荐将 0–0 作为全天唯一同端点表示，其他相同端点在注册前拒绝。 |

BUG-L03-010 的 enabled 用例本轮实际运行并 PASS 1/1，但它正是把矛盾行为作为期望：返回总量 1440 分钟/86400 秒，列表只有一个值 0。它证明当前状态可复现，不证明测试 oracle 正确。原始输出、XML、命令、独立 cwd/Profile 和目标 SHA 见 evidence/02_models_tools/l03-010.*。

### p12_datafactory_datadef

| 编号 | 裁定 | 结论 |
|---|---|---|
| BUG-P12-002 | PRODUCT | 公开 setPeriod 可写入 times=0，updateSecData 用 times 作除数；独立公开 extractKlineData(seconds) 也直接以 seconds 作除数。两入口都必须防零，不能只在 setter 修一个路径。旧危险用例是 DISABLED，本轮未启用。 |
| BUG-P12-005 | NEEDS_DECISION | WTSBarStruct 的 add 与 option ask 共用 union。期货日线 add 表示持仓变化，期权 ask 是价格快照；extractDayData 是通用 API，没有 instrument kind/field mode。当前测试没有 add=6/16/11 或期权 ask 聚合断言，只能说明字段语义仍未区分；合同确认前不把任一聚合方式作为通用 oracle。 |

P12-005 当前没有针对 add/ask 语义的本轮运行结果。已有旧 evidence 不属于当前合同/断言的验收证据，因此不列为该条的 `execution_refs`；本报告不声称当前测试新增了 6/16/11 断言。

## 测试修正与建议

1. BUG-L03-010：当前 zero_length_section_is_treated_as_full_day 用例的总量与列表预期互相矛盾。保留 0–0 全天用例；把 900–900 改成受控拒绝或同一零时长策略的自洽断言。
2. BUG-P12-005：测试当前没有 add/ask 分类型聚合断言。待 owner 明确通用 API 的字段/品种语义后，再按选定合同设计对应对照测试；合同未定前不新增 6/16/11 或期权末值 oracle。
3. BUG-R12-12：新增 create/setCode 字节容量边界用例。CodeHelper HOT/2ND wrapper 现已有完整 std::string 相等与 size 断言，不再提出重复修改请求。
4. BUG-L03-005：disabled 用例注释不应把未挂接 builder 对象描述成已确认产品缺陷，也不应作为无 commInfo fallback 的合同。
5. BUG-L02-009、BUG-L03-006、BUG-L03-009、BUG-P12-002：triage 提供了各自最小定向回归；在产品边界修复后执行，超限/零周期危险输入不直接用崩溃测试验证。

所有测试更改已按审计规则发给主代理；本报告没有修改测试、C++ 或构建文件。

## 本轮实际执行

- CoverageTests --gtest_filter=l03_session.zero_length_section_is_treated_as_full_day：PASS 1/1，0.271s；目标 SHA-256 50e6a8019a6192cff4ceaee707dd8886181e5b19a97070a13d4a06b082c063e3。使用独立临时 cwd、LLVM_PROFILE_FILE 与 XML 输出。该断言通过仍显示业务视图不一致。
- P12-005：本轮未运行可用于确认 add/ask 聚合语义的测试；旧 evidence 未作为本轮或当前语义合同的验收结果。
- 其他十条均为静态源码、接口、配置来源、正常调用与测试审核。没有运行 ASan/UBSan/全量测试，没有构建。

所有 Python 子进程控制均按仓库约束使用 /Users/yunjinqi/opt/anaconda3/bin/conda run -n base python。完整运行元数据位于 evidence/02_models_tools/runs.json。
