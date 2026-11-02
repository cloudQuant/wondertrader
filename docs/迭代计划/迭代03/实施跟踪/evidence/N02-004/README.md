# BUG-N02-004 当前源码定向复验

迭代03冻结验收：路由规则缺失/null/类型错误安全拒绝且不部分发布，字符串与数组规则可正常登记。

在 `/tmp/wt-i02-fresh-20261101-v8-build` 提取 `CoverageTests_wtcore` 的 Ninja 编译/链接命令；将 `WtExecMgr.cpp` 和 `n02_exec_mgr_test.cpp` 改为当前工作区源并分别重编至 `/tmp/wt-i03-n02-focused`，链接时将对应两个旧对象替换为这两个新对象，其余对象与静态/动态库复用 v8。具体 argv 及输出见 `commands.json`、`build-0.log` 至 `build-2.log`。两个 .d 均实际包含实体 `cov_boost_include/boost/config.hpp`；该 boost 目录不是符号链接，输出目录无符号链接。`product-symbol.txt` 确认产品路由函数定义在目标可执行文件内，但不证明所有依赖是当前源码或完全插桩。三份当前相关源码编译前后及可执行文件 SHA-256 见 `hashes.json`；其余依赖未做全量指纹证明。

每例在独立 cwd 和 TMPDIR 下运行，限制 30 秒，详见 `results.json` 和 `run0..2/{output.log,result.xml}`：

- `N02ExecMgrTest.RouterRuleWithoutExecuterNodeIsRejected`：PASS，exit 0；
- `N02ExecMgrTest.MalformedRouterRulesDoNotPartiallyReplaceExistingRoutes`：PASS，exit 0，涵盖缺失、null、数值、对象、数组非字符串、无效规则条目及策略类型；
- `N02ExecMgrTest.LoadRouterRulesAcceptsStringAndArrayExecuters`：PASS，exit 0。

3/3，无跳过；仅 macOS arm64 当前两个重编 TU 的定向行为证据，未使用 ASan/TSan，未验证并发配置、Windows/Linux、真实交易接口和当前完整工作树，不代表整体验收。无修改仓库文件，无新增符号链接。本轮未跑修复前红控或整套回归。
