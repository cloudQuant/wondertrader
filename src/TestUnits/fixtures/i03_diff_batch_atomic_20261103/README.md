# 迭代 03：Diff 目标批次原子性定向 fixture

`atomic.inc` 与既有小数 fixture 一起编入隔离的 `CoverageTests_wtcore`。在仓库根目录执行以下脚本创建全新实体快照、构建并运行定向用例：

```sh
src/TestUnits/fixtures/i03_fractional_targets_20261103/run.sh /tmp/i03-diff-batch-new
```

参数必须是尚不存在的绝对路径；脚本拒绝含符号链接的源，隔离构建最多 `-j 2`，不连接交易渠道。它运行两个 fixture 的合并过滤集；本子任务独立的 17 项过滤集另见本目录 `filter.txt`。

测试使用受控 barrier 和替身，覆盖全批溢出拒绝、提交前增量优先、在途旧批新引入代码被后批遗漏、批量持久化失败、空代码、相同目标、未就绪 omitted 通知，以及现有恢复与 trade 排队顺序。针对注册前重入、相同目标及恢复，测试通过 friend 直接构造生命周期状态、调用真实 `registerCreatedUnit` 辅助函数并驱动队列；**这不是动态 factory 或插件完整 initialize 链的验证**。持久化失败为注入，不代表真实文件系统故障；真实 provider、OOM 和完整生命周期仍未验证。
