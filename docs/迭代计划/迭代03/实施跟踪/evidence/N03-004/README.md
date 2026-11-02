# BUG-N03-004 UFT custom-only 风控配置定向复验

按迭代03冻结条目，验证 active riskmon 缺省 default 的自定义配置不会崩溃、已配置合约命中对应规则，并覆盖 default 组对未列品种生效的对照。

从 `/tmp/wt-i02-fresh-20261101-v8-build` 的 `CoverageTests_uftcore` Ninja 命令提取参数，独立重编当前 `src/WtUftCore/TraderAdapter.cpp` 和 `q04_trader_risk_limits_test.cpp`，链接时替换目标的两个对应对象，其余对象和库复用 v8。完整执行 argv 在 `commands.json`，构建输出在 `build-0.log`～`build-2.log`。两个依赖文件均引用实体 `cov_boost_include/boost/config.hpp`；该 Boost 目录及本输出树均不含创建的符号链接。`product-symbol.txt` 确认目标可执行文件定义对应 `TraderAdapter::getRiskParams` 符号。四个相关源码文件在构建前后的 SHA 与最终二进制哈希见 `hashes.json`，不代表其余旧对象与当前工作树一致。

每个用例在独立 cwd/TMPDIR、30 秒超时中执行，命令、退出码和 XML 信息见 `results.json`，原始日志和 XML 位于 `run0`、`run1`：

- `custom_only_policy_matches_configured_products_and_leaves_unknown_products_unmonitored`：1/1 PASS、exit 0。只配置 custom 组时，已配置合约命中风控；未匹配的合约查询安全返回并按当前实现不受该配置监控。
- `risk_params_default_group_is_used_for_unlisted_products`：1/1 PASS、exit 0。未列品种回落 default 并按设置触发相应限额。

2/2 PASS、无跳过；使用 fake trader 和复制的实体插件，无真实下单或供应商连接。该结果直接覆盖风险检查函数及 fake 回报输入，不能证明真实委托/撤单入口端到端阻断；未做 ASan/TSan、全依赖插桩、跨平台或当前完整树验收，也未运行整体回归。
