# I03-NEW-045：WtRunner 进程入口遇到隔离 parser 时的退出（迭代03 s5-runners）

只用本地 fake：无供应商 SDK、无网络、不下单。

- `uncertain_provider.cpp`：直接复用 `../i03_dt_parser_host_20261104s5/fake_provider.cpp`，仅在加载时把 `wait_drained` 固定为 ERROR，得到一个“建过 SDK 对象且无法证明回调排空”的 V2 parser，按卡9 9a 被宿主隔离。
- `run.sh <WtRunner 可执行体> <libWtDataStorage.dylib> <新输出目录> [label]`：编译该 provider，生成最小 CTA 配置（全天交易时段按卡2 写 0-0），同步运行真实 `WtRunner` 进程入口，3 秒后发 SIGINT，最多等待 20 秒，记录退出码与 `Logs/` 中“retained until process exit”诊断行数。

期望：改前 runner 在 `main` 返回时析构，`shutdown()` 因隔离 parser 返回 false 而 `std::terminate`（SIGABRT，状态 134）；改后 `main` 在控制线程有限重试关闭，确认只剩无排空证明的 parser owner 后输出诊断并刻意不销毁 runner（owner 与其借用的 runner 图保留到进程退出），退出码按运行结果返回。fake 只证明本地合同，真实供应商回调排空仍为外部条件（X）。
