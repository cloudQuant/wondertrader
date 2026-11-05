# 异常成交回报隔离：本地复核

2026-11-03 主代理独立重编 `TraderAdapter.cpp`、`UnresolvedReportJournal.cpp`、真实动态加载的 fake provider 和测试，局部 UBSan 集成测试 8/8 通过；`i03-frequency-root-resume.log` 为既有频控 fixture 的复跑结果。其余项目静态依赖复用 `/tmp/i03-quarantine-build-20261103`，并非完整整合树的冷构建。精确命令和测试范围见仓库内 `src/TestUnits/fixtures/i03_report_quarantine_20261103/README.md` 与 `src/TestUnits/fixtures/i03_frequency_20261103/README.md`。

`final-source.sha256` 记录本次源码和测试输入；三个 patch 相对于工作区原有脏文件的任务开始快照，并不是相对 Git HEAD 的完整改动。`source-check.log` 记录独立运行前后的源码校验。

本次仅验证单个 adapter 收到的无法识别 WTS 成交回报。配置 journal 后启动保持暂停，人工对账恢复尚未实现；订单与持仓回报、provider 内部丢弃的 SDK 原始消息、多 adapter 共享账户门禁、真实柜台、硬断电和其他平台均未完成验收。失败时若连内存快照本身也无法分配，代码保持暂停但不能保证留存事件；硬断电时未落盘事件也可能丢失。
