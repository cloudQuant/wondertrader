# Manifest — n05_exec_factories（lane N05：WtExeFact / WtExecMon / WtRiskMonFact）

## 新增测试文件（src/TestUnits/coverage/n05_exec_factories/，共 5 个，40 个 TEST，其中 1 个 DISABLED）

| 文件 | 用例数 | 覆盖符号/行为 |
| --- | --- | --- |
| n05_ordmon_test.cpp | 12 | WtOrdMon::push_order/erase_order/has_order/check_orders/clear_orders/enumOrder：重复 id 覆盖、bCanCancel=false 跳过、超时边界（`>= expire*1000` 才回调）、空表短路 |
| n05_exe_fact_test.cpp | 9 | WtExeFact：createExecFact/deleteExecFact（C 接口）、getName、enumExeUnit（TWAP=普通/MinImpact=特殊）、createExeUnit 全部 5 个注册名、createDiffExeUnit、createArbiExeUnit 恒 NULL、deleteExeUnit 的 NULL/冒名拒绝分支、各 ExecuteUnit 构造+getName/getFactName+析构 |
| n05_minimpact_calc_test.cpp | 9 | WtMinImpactExeUnit 算法核心（FakeExecCtx 驱动 do_calc）：init 参数读取、无 tick 不发单、按 lots 开仓买入（对手价+offset*pricetick）、byrate 比例=round(askqty*rate) 并按目标差额截断、pricemode=2 自动取失衡侧一档价、clear_all_position 强制平仓卖单（force=true）、反向未完成单先撤后补（撤单回报计数 _cancel_cnt 归零才重算）、expire 超时 on_tick 撤单+回报后追单、涨跌停价格修正、on_entrust 失败剔除重算、非本合约 set_position/clear 忽略 |
| n05_channel_ready_test.cpp | 3 | on_channel_ready 三分支：柜台遗留单全撤并按 id 进监控、断线重连后本地幽灵单清空、状态一致仅日志重算；on_channel_lost 空实现 |
| n05_riskmon_datamgr_test.cpp | 6+1 DISABLED | WtRiskMonFact：createRiskMonFact/deleteRiskMonFact、getName、enumRiskMonitors("SimpleRiskMon",isLast=true)、createRiskMonotor 命中/未命中、deleteRiskMonotor NULL/冒名拒绝；WtSimpleRiskMon getName/getFactName/stop（未 run）；WtSimpDataMgr：grab_last_tick/get_tick_slice 空缓存安全返回、handle_push_quote(NULL) 短路；DISABLED 用例收口整个「未 init 即推送/读取」崩溃+UB 输入类（BUG-N05-001/002） |

## 跳过目标及原因
- WtStockMinImpactExeUnit / WtStockVWapExeUnit / WtTWapExeUnit / WtVWapExeUnit 的 do_calc 全量路径：依赖 cpp 内 inline `get_real_target`/`is_clear` 与交易时段模板细节，本轮以工厂+名称+基础回调覆盖，深度算法用例列入下一波（TWAP/VWAP 的时间表推进依赖墙钟与 session 组合，本轮按纪律不断言时间）。
- WtSimpleRiskMon::init/run：需真实 WtPortContext（组合账户资金回调）与后台线程+毫秒 sleep（墙钟相关），跳过。
- WtExecRunner::init/config/run、WtExecPorter C 门面：需真实 cfg 文件、DLL 目录与交易通道，跳过；其内部符号已在 TU 清单内，后续波次可用内存 variant 驱动 config 分支。
- 需真实柜台回报的分支（TraderAdapter 异步 on_entrust/on_trade 落地、registerTimer）：一律以 FakeExecCtx 同步数据替代或跳过。

## 对 TU 清单的需求
- 无新增目标：本 lane 仅调用 WtExeFact/WtExecMon/WtRiskMonFact 三目录内 .cpp 已列入的符号及头文件 inline/Share 既有库符号。若下一波需要 QuoteFactory/CTPLoader 之外的 hidden 模块再另行申报。

## 缺陷记录
docs/迭代计划/迭代01/bugs/n05_exec_factories.md：3 条，全部「待审核」（N05-001 未初始化成员 Major、N05-002 handle_push_quote 空 _s_info 解引用 Blocker、N05-003 check_orders 判空未持锁 Minor）。未修改任何生产代码/CMakeLists。

## 门禁结果
仓库根执行规定 clang++ -fsyntax-only -Wall -Wextra -Werror 命令，5 个文件全部 0 error 0 warning（RC=0），含最终一轮全量复核。
