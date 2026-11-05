# 2026-11-05 回放器独立快照本地复核

独立复制当时源码（复制后 diff -rq 无差异），在 /tmp/wt-i03-replayer-review-20261105-e3UR44Fa 使用默认告警策略、WT_ENABLE_COVERAGE=ON 配置并构建；日志和哈希见本目录。每组使用独立工作目录、超时保护及 Google Test XML。

5 组共 14 次用例执行，14 通过，0 失败，0 跳过；存在与其他批次重复的用例。summary.json 和 runs 下给出过滤器和明细。

- adjusted_keys（4/4）：R2_wtbtcore#2 的复权长短键、独立实例并发键隔离及回归。并发 loader 返回失败，未验证不同键各自返回不同因子。本次为普通构建；旧 LLVM 22 ASan/UBSan 4/4 见 20261104-s5-btcore/README.md，尚未证明旧插桩二进制与此次源码完全一致。
- replay_public、p07_loader、day_price、runner_config：对已标记本地通过项目按该快照再复测。

这些结果不等同于最终工作树的全量回归、CI、跨平台或生产验收；同一实例并发安全不在该 R2 测试合同内。
