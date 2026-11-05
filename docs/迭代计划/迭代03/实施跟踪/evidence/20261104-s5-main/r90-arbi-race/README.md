# I03-NEW-099（R90 部分）：线程池执行器用例首笔委托与后续驱动竞态；假柜台计数非原子

- 现象：`R90ExecPool.ArbiPoolForwardsEverything` 8 路并行逐例偶发 `n_insert` 为 0；串行 40 次全过（`serial-40x-before.log`）；12 路并行各重复 15 次，失败 3/180（`stress-before/`）。
- 归因：首笔委托由线程池工作线程处理首个 tick 时发出。负载下工作线程晚于测试线程随后调用的 `set_position(empty)`/`on_channel_lost` 运行，执行器按“通道已断开”正确拒单，属测试时序问题，非产品缺陷。另 `R71FakeTraderApi::n_insert` 为普通 `uint32_t`，由工作线程写、测试线程读（`r71_local_serialization_test.cpp` 已有跨线程轮询），属数据竞争。
- 修改（仅测试）：`n01_wtcore_tradepaths/r71_wtcore_env.h` 的 `n_insert` 改为 `std::atomic<uint32_t>`；`q01_wtcore3/r90_executer_pool_test.cpp` 的 Local/Arbi 两例在首个 tick 后有界等待（≤5 秒）首笔委托再继续驱动，末尾断言不变。改前副本 `before/`，改后 `after/`，哈希 `sources.sha256`。
- 验证：仅改 Arbi 时 Local 同类失败 2/180（`stress-after-arbi-only/`）；两例均改后 12 路×15 次 360/360（`stress-after/`）。全部覆盖率目标在 -Werror 下编译通过。
