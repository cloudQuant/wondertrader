# R3-008 / Q04-002：订单账本红控与重入对照

2026-11-02，macOS arm64；fake trader，不连接真实交易。此次没有修改产品源，两个条目均未闭合。新测试保存在当前工作树，不要把以下失败计为通过。

## 结果

- WtCore `R59Buy.open_caps_quantity_by_remaining_long_and_total_limits`：exit 1；请求 10、fake 接收 3 时未完成量为 +10，预期 +3。`R59Sell.open_caps_quantity_by_remaining_short_and_total_limits`：exit 1；请求 10、分别接收 3 / 7 时为 -10，预期 -3 / -7。当前测试新增卖向断言。对应的买卖拆单正常控制各 exit 0。见 `core-run0..3.{log,xml}`、`core-results.json`。
- UFT 已有四方向同步 -1 无回调测试 exit 1。新增同合约两笔订单 2 + 3 的重复拒单测试 exit 1：第一次拒单后为 3，第二次相同拒单后变为 1。新增重入对照：fake 在第二笔 `orderInsert` 内同步返回拒单回报，然后返回 -1；另一笔活单仍为 3，当前代码 exit 0。正常四方向组装控制 exit 0。见 `run0..3.{log,xml}`、`results.json`。
- 受控中间候选仅在 UFT `orderInsert` 返回负值时调用现有按合约 rollback（`naive-control.diff`，只在 `/tmp` 构建，未应用到产品）：无回调测试 exit 0，但重入控制 exit 1，另一笔活单从 3 错减到 1；重复拒单测试仍 exit 1。见 `naive-run0..2`。该控制支持“不能直接按合约双重扣减”，不证明某个真实供应商一定会以此时序回调。

## 路径与未闭合问题

WtCore 的 `buy` / `sell` 在限量和拆单前分别按请求量增减 `_undone_qty`；`doEntrust` 生成 localid、传入 API；拒单、撤单、成交回调均有独立聚合扣减路径。UFT 四个 open/close 入口在提交前更新聚合量，`doEntrust` 设置本地标签后调用 API；拒单回调在检查通道标签后只检查合约总量是否非零。两者均缺可证实的逐订单幂等状态。

后续实现需要在任何可能重入的外部调用前登记可确认的逐订单身份与对应数量，并使同步负返回、重入和迟到拒单、成交、撤单按同一订单协调；不能持有会阻塞重入回调的锁跨 API 调用。还要解决快照查询结果与实时回报的排序、订单身份跨重启与回绕、重复回报最迟时间及安全回收边界。当前接口未给出可靠去重保留期限和快照一致性承诺，不能据这些测试推导无界 tombstone 或简单按合约回滚的生产安全性。真实 SDK、跨线程、跨进程、Windows/Linux 与完整整合树未验证。

## 构建与重现范围

`build.py` / `build_core.py` 从既有隔离构建 `/tmp/wt-i03-r78-20261102/build` 和 `/tmp/wt-i03-r42-003-20261102/build` 提取 Ninja 命令，分别重编当前工作区两个 TraderAdapter TU 与相应测试 TU，链接时替换对应对象；其余对象和动态库复用 `ccaad010` 的隔离快照构建。`commands.json`、`core-commands.json` 保存实际完整 argv；`build-*.log`、`core-build-*.log` 均 exit 0。源码前后哈希一致，见 `source-{before,after}.sha256`、`core-sources-{before,after}.sha256`；最终二进制哈希分别保存在 `binary.sha256`、`core-binary.sha256`。Boost 头使用 `/opt/homebrew/Cellar/boost/1.88.0/include`，未创建符号链接。编译链接和测试均未使用 sanitizer；仅当前重编 TU 来自当前工作树，其余旧依赖不代表最终整合树。

`run.py`、`run_core.py` 和 `run_naive.py` 逐测试进程使用独立目录与 30 秒 timeout。`naive.py` 产生临时候选并记录 `naive-commands.json`。各脚本通过 `/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python <script>` 执行；这些脚本依赖上述临时隔离构建存在，不能直接作为自包含的干净构建证明。

## 追加：回报、撤单与快照重入反例

对重新编译的当前 UFT 源与测试单元另运行 `run_events.py`，逐例 30 秒上限，见 `events-results.json` 和 `events-run0..2`；三项均 exit 1：

1. 同一成交 ID 的两次通知，活单 5 先变 3，再错误变 1。
2. 一笔 5 被撤销并另有一笔活单 3，相同撤单通知第二次将 3 错减为 -2。
3. 快照构造于成交之前，实时成交先使 5 变 3，随后递交该旧快照，账本重新变 5。测试明确构造旧快照时点；接口未向生产消费者传递该时点，因此测试不暗示消费者可在现行 SPI 中可靠区分新旧快照。

这些用例使用构造的进程内事件，仅说明两者可能交错时的账本行为，不证明某一具体 provider 会产生该时间顺序。重编后原 Q04 四项以及临时候选三项均重跑，结果与本文件前述一致；源文件和二进制哈希已更新。
