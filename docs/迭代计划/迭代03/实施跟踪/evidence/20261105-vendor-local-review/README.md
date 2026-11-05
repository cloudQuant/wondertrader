# 交易插件本地复核（2026-11-05）

本轮单独复制当时工作区 `src` 和 `dist/common` 到 `/tmp/wt-i03-vendor-review-20261105-UBe5fyHN/`。复制完成后 `src` 对比无差异；构建及运行期间原工作区继续变化，运行后对比见 `source-diff-final.txt`。`snapshot-src.sha256`、`binaries.sha256` 保存快照及测试二进制哈希；HEAD 见 `HEAD.txt`，不代表未提交源码内容。

独立使用 Ninja Debug、默认 `-Werror` 和 LLVM 覆盖率选项构建 TraderCTP、CTPMini、CTPOpt、Femas、CTPOptLoader、TraderDumper 和 WtCore 对应测试目标；配置与构建日志见 `configure.log`、`build.log`、`build-dumper.log`。Boost include 指向实体 Cellar 目录；每组单独进程、独立工作目录，单组 120 秒硬限。

`summary.json` 为 28 组检查对应 18 个条目的逐案清单；共 **89 次执行、89 通过、0 跳过**，包括重叠用例，不等于 89 个不同测试。普通测试的 stdout 与 XML 位于 `runs/<组名>/`；风险监控器的局部 ASan/UBSan 与 LSan 结果位于 `riskmon/`。不可证明排空、按合同隔离常驻的监控器用例关闭 LSan，其余两例启用 LSan。受控 fake SDK 的通过不代表真实厂商回调排空、跨平台、完整账户共享/恢复或最终整合验收。

运行后的工作区变化目前涉及回放器及其测试，交易插件源码与此快照的一致性仍需按晋升时逐项核对。整体回归仍为 `NOT_RUN`。

## 按本机合同晋升

对照原条目、已裁定合同和本轮 XML 后，以下六项标记**本地验证通过**；具体产品与测试源文件在晋升前逐一与当前工作区对照，哈希见 `promotion-sources.json`，不代表完整依赖图或最终树一致：

- `BUG-T25-005`：有终帧、错误终帧、无终帧的 fake 查询停止 3/3；覆盖有界停止、join、一次 Release 与迟到回报拒绝。
- `T3-006`：CTPMini 缺配置、缺库/符号、空 creator、fake 成功与失败后重试等 7/7。
- `BUG-T4-003`：Femas 同步回调重入、失败及生命周期 fake 场景 13/13。
- `T1-CTPOpt-01`：期权行权查询多帧合并、下一批隔离、释放和引用归还 2/2。
- `T2-CTP-07`：订单状态相关 4/4，另 instrument 状态 0..6 1/1；保留真实 SDK 与外部消费者兼容限制。
- `T3-012`：零刷新间隔拒绝、单次运行与正间隔有界停止，加生命周期 fake 正反控；真实阻塞 provider 与回调排空仍未验。

其他 8 项保持原状态，仍可能缺本机条件或完整生产链。例如 CTP 的双无 tag 身份、CTPOpt 行权撤单与释放并发、CTPOptLoader 异步末包宿主链及跨 adapter 账户恢复不能只由当前局部用例证明。

后续补跑 AD 连续合约分段读取 6/6、Runner fake 交易通道绑定/失败清理 5/5、执行器关联回归 8/8、风险监控器专项 3/3。规则真实数据、并发和资源边界、非 AD 读端、真实交易通道、profile 运行时泄漏归因及外部风控监控器等限制依各条台账保留。

`BUG-R104-003` 按原本机验收复核后记本地验证通过：`runner_traders` 5/5 覆盖有效模拟交易器绑定 local/diff、缺失模块不绑定及失败后的 API 清理。测试未连接或登录交易通道；后续失败时首个 adapter 保留为惰性对象，不能表述为注册表清空。晋升前关键源码的对照哈希见 `promotion-sources-runner.json`。

`BUG-I02-NEW-034` 保持待验证：现有风控专项覆盖同步停止、隔离与重复初始化，但首次初始化失败和可重入 shutdown 的本机正反控尚未补齐。不可证明排空的隔离用例关闭 LSan；其余两例启用 LSan。
