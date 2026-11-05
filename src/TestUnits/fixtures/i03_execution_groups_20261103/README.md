# 迭代 03：Local / Arbi 组合目标定向 fixture

从仓库根目录运行：

```sh
src/TestUnits/fixtures/i03_fractional_targets_20261103/run.sh /tmp/i03-execution-groups-new
```

输出目录必须不存在。脚本实体复制工作树源码，在隔离副本裁剪测试目标，以 `-j 2` 构建，运行本 fixture 与原有小数、Diff 原子性控制；独立组过滤集见 `filter.txt`。测试使用 fake unit 和 commodity stub，不连接交易渠道。

组配置支持有序数组，旧 object 中重叠的组要求显式完整 `group_order`。计算按配置顺序处理带符号和小数权重；显式组不重新分解，物理净目标包括显式组的展开量。`group_limits` 限制缩放后组目标绝对值，零有效。Local / Arbi 测试覆盖上下限、做空禁止和缺元数据、验证期间重入引发的版本变化拒绝、未发布的非法组配置，以及旧单组控制。

该变更保留现有执行单元路由：组合代码仍交给配置的执行单元；仓库默认内置执行单元并不自动支持自定义组名，本 fixture 的 fake unit 不证明真实组合可成交。账户级每物理合约净持仓限额、跨合约原子成交、真实 provider 和跨平台尚未验证或实现。本 fixture 的本地验证快照早于上游加载器的独立 NUL 修复；该修复待在最终集成树重新验证。加载器仍可能在执行器读取前丢弃 null 对象字段或数组元素，重复对象键沿用后值，因此本次不能声称对原始配置完成严格 eager 校验。非空但未显式设置交易模式的 commodity 可能默认 `TM_Both`，执行器仅凭 `canShort()` 无法确认该模式是否明确配置。
