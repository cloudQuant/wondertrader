# BUG-R10-005 当前源码局部定向复验

2026-11-02，macOS arm64。运行 `bash 'docs/迭代计划/迭代03/实施跟踪/evidence/R10-005-20261102/build.sh'`，退出码 0，实际编译命令与编译输出见 build.sh、build.log。该构建重新编译 EventNotifier、WtHelper、两份测试源码和三种受控 MQ 模块；WTSTools、WTSUtils 两个静态库复制自 `/tmp/wt-i03-r41-drain/libs/` 的既有构建，见 `inputs-before.sha256`，不能证明其来自本次完整源码快照。Boost 头使用实体 Cellar 目录，本次未创建符号链接；`symlinks.log` 仅检查该 Boost include 和本次临时构建目录。所列关键源和预编库在运行后 hash 未变化，见 `input-check.log`；该列表不是完整依赖闭包。

运行 `bash 'docs/迭代计划/迭代03/实施跟踪/evidence/R10-005-20261102/run.sh'`，退出码 0。4 个用例分别在独立临时工作目录运行，分别以 30 秒 alarm 限时，全部退出 0，XML 均为 1 test、0 failure、0 skip：

- 缺 regiter_callbacks 时卸载候选，既有 notifier 状态及事件发布保持可用。
- 缺 create_server 时拒绝、卸载、无 worker，附完整 fake 正控。
- fake 完整模块验证 create/publish ABI、默认 confirm=false、创建/销毁及事件发布。
- 异步订单发布阻塞时保留对象，发布返回后析构一次。

各自 stdout 和 XML 位于 `runs/`。没有更改产品代码。完整 MQ 客户端接收全部通知主题未在这次运行，真实 provider、跨平台、完整干净构建和整合回归均未验证。关闭排空、回调/owner 生命周期和无法返回的 publisher 边界由 R41-003 等关联事项继续跟踪；fake 模块不证明真实 provider 合同。
