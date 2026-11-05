# NEW-034：资金对象泄漏与共享监控器生命周期复核

2026-11-02，macOS arm64。**仅是风险和受控测试证据，没有产品修复或验收。** 没有真实 provider、交易或新建符号链接。

现有 `WtEngine::load_datas()` 调用 `WTSPortFundInfo::create()`，并覆盖 `_port_fund`；`getFundInfo()` 返回该裸指针。源码的 `WtEngine::release()` 排空其 task worker，但没有调用 `_risk_mon->self()->stop()`；`setRiskMonitor()` 接收共享 wrapper，外部可以继续持有。内置 `WtSimpleRiskMon` 读取该裸指针。外部监控器对指针是否 retain、何时停止及其 `stop()` 是否排空均无已验证合同；不能仅据局部测试假定可安全释放或替换。

运行：

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python docs/迭代计划/迭代03/实施跟踪/evidence/NEW-034-fund-owner-20261102/run.py
```

脚本复用此前 `../R42-001-20261102/README.md` 中 ASan/UBSan 构建及快照的目标对象、库和链接命令，将本目录独立 gtest 源插桩编译链接；执行前核对 `WtEngine.cpp/.h`、`WtSelEngine.cpp/.h`、`WTSRiskDef.hpp` 和 `RiskMonDefs.h` 六个当前文件与该快照字节一致。其他源码和预编库未全量核对，不是对当前完整树的重编；旧 `/tmp/wt-i03-r42-001-20261102` 构建若被清理则脚本无法运行。系统库及预编 GTest 等未全部插桩。初始构建脚本尝试分别因编译数据库中无 `-MF` 和系统 linker 不支持旧命令选项失败，改为可选 flag 及固定先前 LLVM/Homebrew PATH 后成功。

* `observer.log`：真实 `WtSelEngine` 初始化后通过公开 `setRiskMonitor()` 设置可被外部保留的 fake wrapper，fake 从 `getFundInfo()` 获取裸指针；调用 `release()` 和 engine 退出后 fake 的 `stop()` 次数均为 0，外部 wrapper 及其指针值仍存在。断言 1/1，进程退出 0，启用 ASan/UBSan/LSan。测试在销毁 wrapper 后**手动释放当前引擎泄漏的引用**，避免控制用例泄漏；未在释放后解引用指针，也未启动 monitor 线程，不能证明第三方插件会保留此指针或生产并发行为。
* `initialization.log`：复用先前 SEL 正常初始化用例，断言 1/1 后进程退出 1，LSan 报告来自 `WTSPortFundInfo::create → WtEngine::load_datas` 的 144 字节直接泄漏。这是预期红控，不计测试通过；`LLVM_PROFILE_FILE` 未设置。

两个结果对照说明：必须先明确和实现 owner 对风险监控的停止及排空、外部共享 wrapper 的生存期，以及资金借用或 retain/release 合同，再在适当安全时点释放资金引用；重复加载时同样需要明确定义旧指针的存续期。在没有这些证据前，没有修改 WtEngine 的析构、release 或 load_datas，也不关闭 BUG-R42-001/NEW-034。
