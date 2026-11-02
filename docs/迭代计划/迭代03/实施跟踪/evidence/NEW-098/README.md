# BUG-I02-NEW-098 CTA 工厂和 DualThrust 定向复验

从 `/tmp/wt-i02-fresh-20261101-v8-build` 的 `CoverageTests_ctafact` Ninja 命令提取参数，独立重编当前 `n06_cta_stra_test.cpp`、`n06_cta_ctx_mock_test.cpp`、`WtCtaTwoMaFact.cpp`、`WtStraTwoMa.cpp` 与 `WtCtaStraFact/WtStraDualThrust.cpp`，替换 v8 对应的五个对象后重链；其他对象及静态/动态库复用 v8。目标编译链、链接 argv 见 `commands.json`，各构建输出见 `build-0.log` 至 `build-5.log`，全部退出 0。五个 .d 均包含实体 `cov_boost_include/boost/config.hpp`；重编 TU 未包含 `ModuleHelper.hpp`，但其余旧对象仍可能使用旧头。`product-symbols.txt` 核对当前工厂枚举与 DualThrust 会话/调度函数在目标二进制定义；相关源码/头构建前后及复核时 SHA 一致，见 `hashes.json`，不证明剩余 v8 依赖与工作区一致。未创建符号链接。

三个指定单例在各自隔离 cwd/TMPDIR、30 秒超时进程中运行，各 exit 0/1 PASS、无跳过，日志和 XML 在 `run0..2`，具体命令见 `results.json`：

- `n06_cta_fact.enum_strategy_reports_one_name`：明确断言枚举恰好一次，工厂名称匹配、策略名称为 `TwoMa`、末项标志存在，且该名称可创建和删除同名策略。**此测试编入的是 TwoMa 工厂，未验证 WtCtaStraFact 的 DualThrust 工厂枚举**。
- `n06_cta_ctx.dualthrust_on_session_begin_switches_main_contract`：断言旧 `SHFE.au.2005` 清零带 `switchout`，新 `SHFE.au.2006` 恢复 3 手带 `switchin`，主力不变时不重复操作。
- `n06_cta_ctx.dualthrust_on_schedule_breaks_upper_enters_long`：按回调顺序设置主力，断言发单对象 `SHFE.au.2005`、数量 2、方向 `enter_long`、标签 `DT_EnterLong`，并检查上下轨指标存在。

没有运行任何其他测试或 disabled 用例；本轮未跑同进程关联组。仅限 mock 策略上下文、本机单次定向执行，不证明真实行情/交易、其他策略工厂、跨平台、完整树或整体回归。
