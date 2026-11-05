# CTA / SEL 回测费用消费定向验证

在仓库根目录执行：

```sh
bash src/TestUnits/fixtures/i03_backtest_fee_consumer_20261103/run.sh /tmp/wt-i03-ctasel-fee-20261103
```

脚本在独立目录构建当前 `WtBtCore`、`WTSTools`、`WTSUtils`（`-j2`），编译真实 CTA/SEL mocker 与 Replayer 链路测试和原 p03 正常交易控制；复制一个实体 nanomsg 动态库到构建目录，检查构建目录无符号链接。依赖本机 macOS 的 `/opt/homebrew`、`/opt/local` 和 `/usr/local/lib/libnanomsg.6.0.1.dylib`，不是跨平台脚本。整个脚本最后一次运行退出码 0。

`evidence/fee-consumer-final.log` 记录 23/23，覆盖 CTA/SEL 的合约优先、品种回退、合法零、缺费/不完整费表、分段平今平昨及反手、后段计算/累计费溢出拒绝、有限大数量反手、连续码来源缺失、条件单回调以及从真实 Replayer HFT 事件分发两笔不同物理合约 tick 到 mocker 的成交。`evidence/p03-final.log` 记录迁移原 p03 费表与合约元数据后的 7/7 正常控制，包括滑点和 T+1。`evidence/build.log` 和 `evidence/source.sha256` 对应上述脚本的最后一次构建；`baseline/` 为本轮修改前四个 mocker 文件和两个 p03 测试的副本与哈希。

初次功能测试的夹具未初始化合约乘数，导致 10 项失败；补齐合约乘数后通过。真实 Replayer dispatch 测试初次未提供 session/holiday 基础数据，导致 2 项失败；补齐基础数据后通过。此前一次 p03 编译发现 disabled 旧探针仍引用旧费率 helper，恢复 helper 后编译通过；该 disabled 探针没有运行。

手续费拒绝发生在账本交易提交前；价格缓存和已发生的动态浮盈更新不属于此事务保证。此前使用空费表或直接填 `_fee_map` 的其他旧测试可能需要迁移，未运行整个旧测试集。未运行真实 provider、真实交易、全量回归、跨平台或完整 sanitizer；本 fixture 不证明对外回放完成状态或 HFT/UFT 消费链的验收。
