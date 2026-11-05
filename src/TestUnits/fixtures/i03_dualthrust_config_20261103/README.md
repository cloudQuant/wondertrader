# Iteration 03 DualThrust 配置测试

从仓库根目录运行以下命令。fixture 使用独立的 `/tmp/wt-i03-dualthrust-config-20261103` 构建目录，不依赖公共 CMake 目标；需要 CMake、C++17 编译器、fmt 和 GoogleTest。以下命令与留存记录使用的路径、过滤条件及并行度一致：

```sh
cmake -S src/TestUnits/fixtures/i03_dualthrust_config_20261103 -B /tmp/wt-i03-dualthrust-config-20261103 -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-dualthrust-config-20261103 -j2
ctest --test-dir /tmp/wt-i03-dualthrust-config-20261103 --output-on-failure
/tmp/wt-i03-dualthrust-config-20261103/i03_dualthrust_existing --gtest_filter='n06_cta_ctx.dualthrust_undefined_short_windows_wait_without_signals:n06_cta_ctx.dualthrust_on_init_subscribes_and_registers_index'
```

`evidence/build.log` 记录扩展 TwoMa fixture 之前的配置和增量构建，退出码 0；`evidence/config-test.log` 记录独立配置测试 1/1 通过、退出码 0；`evidence/existing-test.log` 记录上述两个现有过滤用例 2/2 通过、退出码 0。`evidence/sha256.txt` 是这一先前构建时部分关键源文件、fixture 源文件和本地二进制的哈希；其中 CMakeLists 和二进制哈希不代表后续扩展后的构建，且该文件不是所有传递依赖的清单。`baseline/` 保存本轮修改前的策略源码及受授权修改的现有测试副本，`baseline/sha256.txt` 为其哈希。此前一次中间 fixture 编辑笔误导致编译失败，修正后的结果见上述日志。

扩展后的 fixture 另编入现有 CTA 参数测试和 TwoMa 工厂（由工厂提供 `FACT_NAME`），使用独立构建目录运行以下五项参数、历史及模拟正向交易测试；本次本机结果为 5/5 通过，配置、构建和测试退出码均为 0：

```sh
cmake -S src/TestUnits/fixtures/i03_dualthrust_config_20261103 -B /tmp/wt-i03-dualthrust-twoma-20261103 -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-dualthrust-twoma-20261103 -j2
/tmp/wt-i03-dualthrust-twoma-20261103/i03_dualthrust_existing --gtest_filter='n06_cta_stra.init_rejects_missing_or_zero_two_ma_periods:n06_cta_stra.init_rejects_two_ma_history_shorter_than_window:n06_cta_stra.init_rejects_two_ma_periods_outside_signed_index_range:n06_cta_ctx.twoma_on_schedule_golden_cross_enters_long:n06_cta_ctx.twoma_short_window_needs_more_history_and_releases_slice'
```

兼容边界：`days` 现在必须显式提供正整数；`days=1` 仍接受，但当前调度逻辑等待而不生成信号。`count` 必须为正整数，`period`、`code` 必须为非空字符串；显式 `k1`/`k2` 必须为有效有限数值，缺省系数仍取零。整数值支持十进制和科学计数法，非法值不会先发布配置成员。旧测试中的 `days=0` 初始化成功断言已依据卡 8d 改为拒绝断言；disabled smoke 中 DualThrust 部分改为使用有效配置并检查初始化结果。

本证据仅覆盖所列本机功能测试；完整回归、其他策略的参数合同、真实 provider、真实交易及跨平台验证均为 `NOT_RUN`。本目录中的测试结果不代表整项缺陷验收。
