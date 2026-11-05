# R60-003：已接收队列和 owner 生命周期复核

本次没有改动产品源码和仓库测试；`probes.inc` 经 `prepare.py` 插入当前 R60 测试副本，仅在 `/tmp/wt-i03-r60-queue-audit` 编译运行。当前产品、测试和 owner 源码哈希见 `source.sha256`。产品与测试 TU 独立重编，链接复用 `/tmp/ctpm-i03-build` 的依赖；日志、命令、退出码和 XML 在本目录。不触及真实柜台。完整依赖、其他平台和最终树未验。

- `audit_accepted_queue_without_stop_control`：connect 回调受屏障阻塞，接受一笔未知合约订单与一次账户查询（均返回 0），解除阻塞后分别收到订单错误和查询回报；PASS，exit 0。
- `audit_accepted_queue_stop_does_not_lose_results`：同样先接受订单和查询，在回调仍被阻塞时由外部线程调用 release；确认准入已关闭后解除屏障并等待 release 返回。订单回报 0、账户回报 0、订单引用计数仍 2；该用于检测接收后无结果的断言 FAIL，exit 1。订单 handler 在 io_context 销毁时仍可由现有 RAII 释放引用；本例不声称引用永久泄漏。等待队列允许已有请求在停机时执行，还是返回明确取消、持久隔离及对账，仍需要定义；此反例单独不能选择其中一种策略。
- `release_reentered_from_connect_callback_does_not_terminate`：当前源码既有 fork 隔离反例仍 FAIL，child terminate handler 退出 88，表明不能在自身工作线程同步 join。
- `audit_spi_unregister_does_not_drain_captured_listener`：有效订单回报中阻塞 `onRspEntrust`，外部 `registerSpi(nullptr)` 后恢复，仍收到同一已捕获 listener 的 `onPushOrder`；观察用例 PASS。测试保留 sink 到线程结束，不使用悬空对象。此行为说明清空原子指针并非已进入回调的排空屏障，不主张 `registerSpi` 已提供该合同。

源代码所有权边界：`ITraderApi::release` 返回 void、无完成状态；`TraderMocker` 的 handler 捕获裸 `this`，`deleteTrader` 直接 delete。`WtCore/TraderAdapter::release` 注销 SPI 后调用 release，再调用 remover 并卸载 DLL；manager 在 sink 回调中有单独的早退，但不能据此断言所有直接 adapter/外部 owner 路径都被保护。`WtUftCore` 有 callback 准入/等待，但其自身注释承认不能仅凭 adapter 侧屏障证明 provider 不会迟到。若回调内简单跳过自 join 并返回，旧 owner 可以立即 delete facade、释放 SPI/数据管理器或卸载仍在执行的模块；joinable thread 析构也可 terminate。若只在 release 中执行排队工作，真实 owner 已注销 SPI，可能产生无结果的交易副作用；若直接丢弃则同样缺少明确失败/对账。上述反例与所有权路径均不能由增加 `io_context.restart()` 或静默丢弃 handler 安全修复。

候选整体方案需 owner 与 provider 联合设计、保持旧 vtable：版本化能力表达请求停止与可等待完成的区别；关闭准入并定义每个已接收订单/撤单的执行或可观察失败、查询取消，以及 SPI 已注销时的可恢复对账；callback 内只请求停止，由非回调 owner 保留 facade、SPI、基础数据和模块并等待 worker/matcher/回调均退出后再删除/卸载；完成后才能创建新的独立 generation，并明确账户状态迁移。需覆盖直接 delete、旧 owner、异常/丢失 sink、并发 stop、线程重入和阻塞 callback 等反例，不能以隔离泄漏或 detach 代替所有权。当前委派仅限 TraderMocker 文件，缺少可强制 owner 等待的接口与合同，未实施危险的局部替代；R60-003 继续阻塞。
