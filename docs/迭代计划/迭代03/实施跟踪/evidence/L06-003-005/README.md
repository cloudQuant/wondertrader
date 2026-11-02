# BUG-L06-003 / BUG-L06-005 定向复验

使用 `/tmp/wt-i03-r58-build` 既有构建参数，独立重新编译当前工作区 `MatchEngine.cpp` 和 `l06_match_engine_test.cpp` 后替换原目标对应对象重链。完整 argv 在 `commands.json`，相对于 `/tmp/wt-i03-r58-build` 执行；实体 Boost bridge 优先。关键源码、头与测试在构建前后及运行后哈希一致，见 `inputs.json`；其他对象与库复用旧快照。

四个定向单例各 exit 0、各 1/1 PASS、无 skip：`cancelByCodeAndSideCancelsOnlyMatchingOrdersWhenQuantityIsZero`、`cancelPositiveQuantityUsesOldestOrdersUntilRequestedQuantityIsReached`、`cancelPositiveQuantityKeepsTimeOrderAfterMatcherErasesEarlierOrder`、`ticksActivateAndMatchOnlyOrdersForTheirOwnContract`。前三项对应 L06-003、最后一项对应 L06-005。每个用例在独立目录执行，命令形态：

```sh
TMPDIR=<独立用例目录> LLVM_PROFILE_FILE=<独立用例目录>/profile.profraw perl -e 'alarm 20; exec @ARGV' -- /private/tmp/wt-i03-l06-current/focused --gtest_filter=MatchEngineTest.<用例名> --gtest_output=xml:<独立用例目录>/result.xml
```

旧 reopened-case-map 将这两项映射至 zeroAskQuantity 测试，该测试未覆盖本项双代码/撤单验收；本次选用直接断言上述行为的用例。并发、其他平台、最终整合工作树及整体回归未验证；未创建符号链接。
