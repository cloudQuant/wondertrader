# BUG-R14-005 HFT/UFT 延迟工具异常传播定向复验

2026-11-01，本机 macOS arm64。对 HFT 和 UFT 各自的产品源文件、main 和 r14/r95 测试翻译单元，用当前工作区源码在 `/tmp/wt-i03-r14-latency` 分别重编并重链；其余 WtCore、基础库和动态依赖复用 `/tmp/wt-i02-fresh-20261101-v8-build`，非完整冷构建。Boost 头优先使用该构建下的真实 `TestUnits/coverage/cov_boost_include`（boost 子目录为实体目录），没有创建符号链接。实际编译/链接 argv 为 `commands.json`，各步输出为 `<HFT|UFT>-build-<0..4>.log`；`build.py` 保存选取并改写 Ninja 命令的方法。

另外以未定义 `WT_LATENCY_TOOL_TEST_HOOKS` 的产品 TU 从当前源码重编并重链 HFT 和 UFT，成功 6/6 编译/链接步骤；命令 `product-commands.json`，日志为 `<HFT|UFT>-product-build-<0..2>.log`。`nm -C` 未发现 `setTestRunFailureForTesting` 或 `test_run_failure` 符号。未直接启动产品进程，入口结果使用定向测试中的改名入口验证。

10 个用例逐进程在各自的工作目录与 TMPDIR 运行，每例超时 30 秒；HFT 5/5、UFT 5/5 通过，均无跳过。`results.json` 逐项记录命令、退出码、XML 属性和工作目录，各目录 `<HFT|UFT>-run<0..4>` 保留输出和 XML。每个工具均覆盖：零样本、一个有效样本、parser/trader/engine 阶段 std::exception 或未知异常的诊断与失败状态、正常入口、注入失败到入口非零状态。测试异常为受控注入，不代表真实交易 API 会以该方式失败，也没有连接真实柜台。

相关两个工具目录和两组测试目录源文件的哈希列于 `hashes-before.json`、`hashes-after.json`，测试后重新核对工作区无漂移。四个产物 SHA-256：
- HFT-focused a34ed19b8805f91e9ddffc825cd8af03113287686f7b488202bb9113e241f9e0
- UFT-focused 77edf78b10a68157c973245998c9c670696274c39319ddade3278c8f3867a025
- HFT-product af5c80a20ada07b4ddf1c11efb6a88c6c00d9d0d0d416606bb0b41d76eb0b7e7
- UFT-product 0f1ac5b9a800c5c25686a7a3ef72e1346f259d43b37fade74c8a35bf057daac7

限制：无全依赖冷构建、sanitizer、Windows/其他平台、真实供应商 SDK、所有异步回调关闭排空或最终整套回归。局部 10/10 不代表整体验收。
