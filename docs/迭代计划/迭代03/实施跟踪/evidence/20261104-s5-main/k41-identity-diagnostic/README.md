# 决策 K41（BUG-N05-003）：未声明宿主订单身份能力的执行插件——接受限制并在加载时诊断

- `src/WtCore/WtExecuterFactory.cpp::loadFactories`：V2 插件加载成功后，若 `capabilities` 不含 `WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY`，记一次 WARN，说明其单元以非共享 owner 模式运行、无自动超时撤单、每次撤单仍须核验 owner 与账本。没有 V2 getter 的旧插件原本就在加载时被拒绝。
- 只增加诊断日志，限制本身（`hasHostOrderIdentity` 判定、非共享 owner、禁止裸 ID 超时撤单）已由既有实现与用例覆盖，因此无红控。日志内容未加专门用例；以全部目标 -Werror 编译与后续全量逐例回归覆盖。
