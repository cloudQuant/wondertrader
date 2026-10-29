# 11_runners 逐条复核与迭代修复分析

日期：2026-10-29  
输入：`/tmp/wondertrader-review-20261029/groups/11_runners.json`（39 条）  
范围：逐条对照候选原文、当前实现、公开接口与测试前置/断言；仅新增本报告和 triage JSON。未运行测试、未构建、未访问网络，未修改原 bugs、测试、生产源码或 CMake/build。候选历史 PASS/FAIL/崩溃记录不作为本轮执行证据。

## 裁定计数

| 裁定 | 数量 | 含义 |
|---|---:|---|
| PRODUCT | 24 | 有当前源码/接口依据的产品路径错误，保留修复项 |
| UNDECIDED | 9 | 行为可见，但输入/调用合同或因果链未证实 |
| DUPLICATE | 0 | 本组没有证据足以合并为完全相同实现与根因的重复项 |
| NOT_BUG | 4 | 当前公开合同或生产可达路径不支持候选主张 |
| TEST | 2 | 仅测试/coverage harness 风险，不是当前生产运行缺陷 |
| **合计** | **39** | 逐条证据、修复和验收见 `triage/11_runners.json` |

PRODUCT/TEST/NOT_BUG 均为 STATIC；UNDECIDED 为 PENDING。本轮没有 LOCAL_REPRO。测试引用只表示静态检查了测试源的前置条件/断言，不能读作本轮 PASS/FAIL。

## 逐条结论

| 编号 | 裁定 | 复核结论 |
|---|---|---|
| BUG-R104-002 | PRODUCT | active parser init 失败仍登记、计 loaded；测试只证明登记，不证明插件加载。 |
| BUG-R104-003 | PRODUCT | TraderAdapter init 失败仍登记，执行器把非空对象当可绑定通道。 |
| BUG-R104-004 | PRODUCT | active CTA 工厂/策略缺失时空指针解引用。 |
| BUG-R14-002 | PRODUCT | 入口忽略 config=false 后仍 run。 |
| BUG-R14-003 | PRODUCT | 四个入口缺 basefiles 段判空。 |
| BUG-R14-004 | PRODUCT | 缺配置仍成功退出；QuoteFactory 现有 0 断言需改。 |
| BUG-R14-005 | UNDECIDED | 空 catch 可见，但可达异常与诊断合同未证实；getchar 是交互行为。 |
| BUG-R14-006 | UNDECIDED | cwd 相对路径可见，产品启动目录合同未证实。 |
| BUG-R14-008 | UNDECIDED | 空日志回调可见，UFT 注册/转发合同未证实。 |
| BUG-R14-009 | PRODUCT | runner 根配置引用未在析构/重复 config 时释放。 |
| BUG-R14-010 | PRODUCT | 公开 bAsync 注释异步，实际 run 无视参数并阻塞。 |
| BUG-R22-001 | NOT_BUG | 公开 C API 为静态单例；内部第二对象不代表多实例合同。 |
| BUG-R22-002 | PRODUCT | C start 只透传 bAsync，allday 默认 false。 |
| BUG-R22-003 | PRODUCT | 缺 basefiles/writer/parser 子键时存在空指针链。 |
| BUG-R22-004 | UNDECIDED | 进程级 logger/module-dir 副作用有，但失败后重试合同未找到。 |
| BUG-R22-005 | PRODUCT | 原始 write_log message 在 raw path 被当 fmt 格式串。 |
| BUG-R22-007 | PRODUCT | createExtParser 忽略登记失败却恒返 true。 |
| BUG-R28-001 | UNDECIDED | stack SIGKILL 无本轮独立 wait status/现场。 |
| BUG-R28-002 | PRODUCT | parse error exit(0)，help 的 0 是正确独立路径。 |
| BUG-R28-003 | PRODUCT | YAML 解析异常未转受控 false，active test 合理。 |
| BUG-R28-004 | TEST | 生产 main 隐式 return 0；风险仅在 coverage 改名 harness。 |
| BUG-R28-005 | PRODUCT | router 缺 executer 时空指针；disabled EXPECT_TRUE 是错预期。 |
| BUG-R28-006 | UNDECIDED | 未知 mocker 留空 listener，但 schema/可达前提待确认。 |
| BUG-R28-007 | PRODUCT | 必需 engine/data init 失败被忽略，config 仍可能成功。 |
| BUG-R6-001 | PRODUCT | 配置 load NULL 后直接解引用。 |
| BUG-R6-002 | PRODUCT | 缺 basefiles 时解引用 cfgBF。 |
| BUG-R6-003 | UNDECIDED | createExtParser 的公开调用时序未写清，栈测试 engine 未初始化。 |
| BUG-R6-004 | UNDECIDED | C API nullability 未定义，现测试同时传两个 NULL。 |
| BUG-R6-005 | NOT_BUG | 只证明内部回调被手工传 NULL，生产可达性未证。 |
| BUG-R6-006 | PRODUCT | 未 config 直接 run 测试不成立；但 config 忽略必需 init 失败后仍可 run。 |
| BUG-R6-007 | NOT_BUG | C Porter 是单例，多实例无公开合同。 |
| BUG-R6-008 | PRODUCT | bool factory API 对普通文件未做目录检查，directory_iterator 可能抛。 |
| BUG-R6-010 | TEST | 重复符号只在假设合并 TU 下，正式目标分开。 |
| BUG-R6-011 | PRODUCT | WtRunner main 忽略配置失败仍 run。 |
| BUG-R6-015 | NOT_BUG | 越界 enum 非合法日志级别，未见生产输入源。 |
| BUG-R95-001 | UNDECIDED | notifier helper 不可达，但 UFT 合同/消费者链未证。 |
| BUG-R95-002 | PRODUCT | UFT config 忽略 initEngine/data 失败并返回成功。 |
| BUG-R95-003 | PRODUCT | times=0 仍计算 total/times，测试不检查结果。 |
| BUG-R95-004 | PRODUCT | UFT parser/trader init 失败仍登记/计数。 |

## 测试误报与修正重点

- `r104_quote_initparsers.active_entries_with_and_without_id_land_in_manager`（`m_QuoteFactory/r104_quotefactory_init_test.cpp:318-350`）对不存在的 parser module 只断言 registry 增长；这证明 active 项被登记，不证明模块加载。需改为可加载 fake 正控与缺模块失败控制，并检查 loaded/ready 状态。
- `r104_wtrunner_config.traders_parsers_and_all_three_executer_kinds`（`m_WtRunner/r104_runner_executers_test.cpp:138-174`）存在不存在的 trader module，却只断言 `config()` 为 true 和 portfolio 文件存在；未断言 adapter ready 或绑定可用。
- `r95_uft_config.*`（`m_WtUftRunner/r95_wtuftrunner_config_run_test.cpp:257-305`）测试注释承认 parser/trader module 加载失败，仍只断言 config true，不能说明插件可用。需改断言并补正控。
- R14-003 的 UFT authored 配置测试（`r14_wtuftrunner_main_test.cpp:154-162`）提供了 basefiles；它验证的是 env/data 缺失，不覆盖整个 basefiles 段。四个入口需分别补缺段用例。
- QuoteFactory 缺文件测试（`r14_quotefactory_main_test.cpp:84-97`）把缺配置返回 0 锁为期望。WtBtRunner 源注释明确 0 成功、非0错误；需保留 help=0，改错误路径断言。
- `r28_wtrunner_config.DISABLED_router_rule_without_executer_key_derefs_null`（`r28_runner_main_test.cpp:954-967`）把必需字段缺失后 `EXPECT_TRUE(config)` 当作目标；应改成 false/no-throw，不能期待无效 router 继续成功。
- YAML parse 与必需 init 的 active 回归（`r28_runner_main_test.cpp:385-400,496-521`）断言受控失败；源码静态显示解析异常未转换、init false 被忽略。旧记录的 FAIL 本轮没有复跑。
- R22 重复 ID 回归（`r22_runner_init_test.cpp:356-393`）唯一 ID 成功、重复 ID 失败的断言符合公开 bool 合同；源码却忽略 manager 的 false。本轮只做静态核对，不将历史 FAIL 复述为本轮结果。
- R6 factory 普通文件用例（`r6_porter_lifecycle_test.cpp:165-184`）要求 bool API 不抛且返回 false，断言方向合理；本轮仅静态确认 `exists()` 后构造 directory_iterator，没有复跑旧报告所述子进程结果。
- R95 零 tick 用例（UFT `:243-258`、HFT `:251-267`）只断言 no-throw/marker，不检查延迟结果是否有限，不能当作零样本统计正确性验收。
- R28-004 的生产 `main` 落尾符合 C++ main 隐式 return 0 规则；候选只描述 coverage 把 main 改名后变普通函数的测试构建风险，当前 `.cpp.blocked` 也不会生成目标。

## 建议修复顺序

| 批次 | 范围 | 门槛 |
|---|---|---|
| 11-A 配置失败安全 | R14-002/003、R22-003、R28-003/005/007、R6-001/002/006/011、R95-002 | 缺文件、语法错误、必需段缺失、初始化 false 均受控失败；未就绪 runner 不得 run。 |
| 11-B adapter/策略 ready | R104-002/003/004、R22-007、R95-004、R6-008 | init/登记结果、ready 状态和 loaded 计数一致；模块/工厂/目录失败不被伪装成功，保留有效正控。 |
| 11-C 接口生命周期/数据语义 | R14-004/009/010、R22-002/005、R28-002、R95-003 | CLI 错误码、配置所有权、异步参数、allday、原始日志与零样本统计均有明确验收。 |
| 11-D 补合同后再裁定 | R14-005/006/008、R22-004、R28-001/006、R6-003/004、R95-001 | 明确异常、工作目录、日志 handler、失败重试、mocker 枚举、parser 时序/nullability、notifier 消费者合同。 |
| 11-E harness 误报 | R28-004、R6-010 | 生产与当前支持目标分开判断；main 改名/合并 TU 风险需独立 coverage 链接验证。 |

## 本轮未验证

未运行测试或构建；未验证 vendor plugin、真实交易下单、回放数据、共享内存、日志现场、异常注入、实际 refcount 归零、子进程信号或 CLI 退出码。结论只来自源码、接口注释和测试前置/断言。需要动态结论时由主代理统一选择定向命令，并附本轮实际命令及结果。
