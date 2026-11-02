# BUG-P08-004 定向复验

使用 `/tmp/wt-i03-r58-build` 原构建记录中的编译/链接参数，在独立目录中重新编译当前工作区 `UftMocker.cpp` 与 `p08_uft_order_position_test.cpp`，并替换原目标中的这两个对象后重链。完整 argv 记录在 `commands.json`，相对于 `/tmp/wt-i03-r58-build` 执行；实体 Boost bridge 优先。编译前后及运行后关键输入未变，SHA-256 和二进制哈希见 `inputs.json`。

独立进程定向运行 `P08UftOrderPosition.CancelAllReturnsEveryAcceptedMatchingId` 和 `CancelAllScansMatchingCodeOnly`，均 exit 0、各 1/1 PASS、无 skip。命令形态为：

```sh
TMPDIR=<独立用例目录> LLVM_PROFILE_FILE=<独立用例目录>/profile.profraw perl -e 'alarm 20; exec @ARGV' -- /private/tmp/wt-i03-p08-004/focused --gtest_filter=P08UftOrderPosition.<用例名> --gtest_output=xml:<独立用例目录>/result.xml
```

其余对象与库复用旧快照，不是当前完整树构建；非活动单、重复撤单语义、跨平台及整体回归未覆盖。未创建符号链接。
