# BUG-Q04-007 UFT 持仓恢复复核

初始旧快照 fake trader 红控 `result.xml`：A 日 4 手、B 日未新成交仍变 8 手。三个补充探针以隔离 TMPDIR 和 fake trader 执行：跨日 realized PnL 保留（probe0，日内清零期望仅依据既有 rollover 实现）、映射失败后同日重试因交易日提前推进而被跳过（probe1）、手工文件未知合约在前导致后续有效项被隐藏（probe2）。均为负控，不等于真实柜台验证。

已单独修复手工导入：跳过未知合约时不占用持久化明细槽，已知明细按 `_size` 连续排列。`results.json` 包括旧对象交错输入失败、新对象通过和现有已知在前正控通过；但修复后原跨日用例仍失败，不能关闭 Q04-007。编译/链接命令及当前相关源码哈希见 `build.log`/`sha256.txt`；其余对象复用 v8。

整项剩余：在保持旧映射有效的同时准备并验证新 position/order/trade 等状态，解决读取与 callback 生命周期、加载失败不推进日期及安全重试；同文件的不同映射不能提供旧状态隔离。多文件崩溃恢复、manual 归档和未知合约语义需明确。`ITrdNotifySink::on_trade` 不带交易日或成交 ID，现有 ready/trade 队列独立，context 锁不能单独证明跨日迟到成交归属或去重。未执行整体回归和真实交易。
