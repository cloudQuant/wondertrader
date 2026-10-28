# lane N06 疑似缺陷记录（策略工厂 / WtShareHelper / WtMsgQue）

## BUG-WSN06-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtCtaStraFact/WtCtaStraFact.cpp、src/WtCtaTwoMaFact/WtCtaTwoMaFact.cpp、src/WtSelStraFact/WtSelStraFact.cpp、src/WtUftStraFact/WtUftStraFact.cpp、src/WtHftStraFact/WtHftStraFact.cpp
- 相关测试: n06_cta_stra_test.cpp `TEST(n06_cta_fact, DISABLED_NULL_NAME_AND_NULL_CFG_CRASH_CLASS)`、n06_sel_uft_hft_test.cpp `TEST(n06_sel_uft_hft, DISABLED_NULL_NAME_AND_NULL_CFG_CRASH_CLASS)`
- 输入/前置条件: 任意 `createStrategy(NULL, id)`
- 期望行为: 参数非法返回 NULL
- 实际行为: 五个工厂的 createStrategy 都直接 `strcmp(name, "...")`，name==NULL 时解引用空指针（UB/段错误）
- 复现步骤: 启用上述 DISABLED_ 用例即可稳定复现
- 影响范围: 策略工厂被上层以配置驱动调用时，缺失/空策略名会打崩进程
- 备注: 修复方向：入口处 `if (name == NULL) return NULL;`。不阻塞覆盖率目标（同类输入已收进同一个 DISABLED_ 用例）。

## BUG-WSN06-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtUftStraFact/WtUftStraDemo.cpp L78、src/WtHftStraFact/WtHftStraDemo.cpp L77
- 相关测试: n06_sel_uft_hft_test.cpp `DISABLED_NULL_NAME_AND_NULL_CFG_CRASH_CLASS`
- 输入/前置条件: `WtUftStraDemo::init(NULL)` / `WtHftStraDemo::init(NULL)`
- 期望行为: 与 WtStraTwoMa/WtStraDualThrust/WtStraDtSel 一致，cfg==NULL 返回 false
- 实际行为: 不判空直接 `cfg->getCString("code")`，NULL 解引用 UB
- 复现步骤: 启用 DISABLED_ 用例
- 影响范围: 演示策略在缺配置时崩溃
- 备注: 修复方向：补 `if (cfg == NULL) return false;`。

## BUG-WSN06-003

- 状态: 待审核
- 严重级别: Blocker（链接期）
- 模块/文件: src/WtCtaStraFact/*、src/WtCtaTwoMaFact/*、src/WtSelStraFact/*、src/WtUftStraFact/*、src/WtHftStraFact/*
- 相关测试: n06_cta_stra_test.cpp、n06_cta_ctx_mock_test.cpp、n06_sel_uft_hft_test.cpp（全部工厂相关断言已按“不依赖链接选择”写成宽松断言）
- 输入/前置条件: CoverageTests 把 5 个插件 TU 直接编进同一个可执行文件
- 期望行为: 各插件符号互不冲突
- 实际行为: 5 个 cpp 各自定义外部链接全局 `const char* FACT_NAME = ...`；WtCtaStraFact 与 WtCtaTwoMaFact 还各自定义同名类 `WtStraFact`（createStrategy/deleteStrategy/enumStrategy/getName 的 mangled 符号重复）以及同名 extern "C" `createStrategyFact/deleteStrategyFact`。单 exe 链接存在重复符号/ODR 风险；即便链接器按 first-wins 通过，deleteStrategy 的 `strcmp(stra->getFactName(), FACT_NAME)` 因两边读取同一个 FACT_NAME 而恒为真，跨工厂删除校验失效
- 复现步骤: 主控真机链接 CoverageTests 即可验证
- 影响范围: 覆盖率可执行文件构建；工厂“非本厂策略拒删”语义
- 备注: 建议修复方向（不实施）：FACT_NAME 改 `static const char*` 或放入各自 namespace；两 CTA 工厂类改名。本 lane 测试因此未断言 FACT_NAME 具体值与“异厂 deleteStrategy 返回 false”。

## BUG-WSN06-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtCtaTwoMaFact/WtStraTwoMa.cpp L149-204（on_schedule）
- 相关测试: n06_cta_ctx_mock_test.cpp `TEST(n06_cta_ctx, DISABLED_ZERO_PERIOD_DAYS_DIVIDE_CLASS)`
- 输入/前置条件: 配置 short_days==0 或 long_days==0（init 不校验），或 K 线条数不足 long_days+1
- 期望行为: 参数非法时拒绝计算或直接返回
- 实际行为: `pre_short_ma/short_days` 除零得 inf/nan；`closes->at(-1*(short_days+1))` 在数据不足时取 INVALID_DOUBLE 参与运算，信号无意义
- 复现步骤: 启用 DISABLED_ 用例
- 影响行为: 回测/实盘产生随机信号（不崩溃，属数值域缺陷）
- 备注: 修复方向：init 校验 `short_days>0 && long_days>0 && count>=long_days+2`。DualThrust 的 days==0 同窗口语义错误，一并收入该 DISABLED_ 用例。

## BUG-WSN06-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtMsgQue/MQManager.cpp L6-18、src/WtMsgQue/WtMsgQue.cpp
- 相关测试: n06_mq_share_test.cpp `TEST(n06_mq_capi, invalid_url_server_survives_and_cleanup)`、`DISABLED_NULL_URL_CRASH_CLASS`
- 输入/前置条件: `create_server("bogus://x", false)` 或 url==NULL
- 期望行为: init 失败应返回 0/无效 id 且不注册
- 实际行为: `server->init(url, confirm)` 返回值被忽略，失败的服务端仍进 _servers；url==NULL 时透传 nn_bind/nn_connect（UB，已收进 DISABLED_）
- 复现步骤: 非法协议 url 调用 create_server 后 publish/destroy
- 影响范围: 上层按返回值判断 server 是否可用的逻辑
- 备注: 修复方向：检查 init 结果，失败返回 0 并判空 url。

## BUG-WSN06-006（派工文本勘误）

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: 派工简报（lane N06 任务书）
- 相关测试: 无
- 输入/前置条件: 任务书提到 `shareblock_helper.h` 与 `CSplitStringPool`
- 期望行为: 引用的文件/符号在仓库中存在
- 实际行为: 全仓不存在 `shareblock_helper.h`、`CSplitStringPool`（find/grep 双重确认，工具结果见会话记录），WtShareHelper 实际由 ShareBlocks.h/.cpp（namespace shareblock）+ WtShareHelper.h/.cpp（C API）构成
- 复现步骤: 不适用
- 影响范围: 排产描述；本 lane 已按真实符号覆盖
- 备注: 建议主控更新任务书措辞。
