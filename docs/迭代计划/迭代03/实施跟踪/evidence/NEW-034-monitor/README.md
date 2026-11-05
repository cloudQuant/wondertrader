# BUG-I02-NEW-034：内置 SimpleRiskMon 停止子项

本项不关闭 NEW-034。`WtEngine::release` 当前不调用 monitor 的 stop；`WtPortContext::getFundInfo` 是裸指针，没有借用/保留时长合同；`setRiskMonitor` 接受共享 wrapper，外部引用可能比 engine 长寿。第三方 monitor 的 `stop()` 也没有证明是排空屏障。因此没有释放或替换 engine 的资金对象，也没有声称 engine shutdown、重复初始化或资金泄漏已经修复。旧版真实 SEL 生命周期 LSan 两处 144 字节泄漏见 `../../../../迭代02/实施跟踪/evidence/20261031-r42-llvm22/lifecycle-strict.log`，2026-11-02 复验见相邻 `../R42-001-20261102/README.md`。

通过代码检查，内置 `WtSimpleRiskMon::_stopped` 在工作线程读、外部线程写而未同步；原 stop 对已 join 的线程再次 join；在 worker callback 内调用 stop 会自 join；并发 stop 也会同时 join。现在用 atomic 停止标志、互斥串行化线程创建/外部 join；线程内 stop 仅请求停止，必须由外部调用 stop 完成 join。停止后不支持同实例重启；与旧实现 stop 后线程指针保留的行为一致。未添加监控器析构内自停或销毁：回调线程自销毁、无外部 join 即销毁、多个 monitor 互相调用 stop 的生命周期均未保证。`WtRiskMonitor` 的虚函数接口未改；`WtSimpleRiskMon` 具体对象布局改变，外部直接构造该具体类型的 ABI 未验证。

## 独立重建

macOS arm64，HEAD `ccaad010`，旧版两个文件来自该 HEAD，修复版两个文件和 fixture 实体复制到 `/tmp/wt-i03-new034/{red,green}/src`，依赖 `src/Includes`、`src/Share`、`src/FasterLibs` 实体复制，复制内容存在工作区既有修改；不是整仓原子快照。`hashes.txt` 可核对当前和测试的产品源/fixture，`symlinks.txt` 为空；无需真实交易。使用 `/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++`、实体 Boost 1.88.0、spdlog 1.15.3、fmt 11.2.0；命令如下，分别把 `V` 设为 red 和 green，并把 `SAN` 设为 `address,undefined` 和 `thread`：

```sh
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -O1 -fsanitize=$SAN -fno-omit-frame-pointer \
  -I /tmp/wt-i03-new034/$V/src -I /opt/homebrew/Cellar/boost/1.88.0/include \
  -I /opt/homebrew/Cellar/spdlog/1.15.3/include -I /opt/homebrew/Cellar/fmt/11.2.0/include \
  /tmp/wt-i03-new034/$V/src/monitor_lifecycle.cpp \
  /tmp/wt-i03-new034/$V/src/WtRiskMonFact/WtSimpRiskMon.cpp -o /tmp/wt-i03-new034/$V/monitor
```

首次编译因快照遗漏 FasterLibs 失败，`compile.log` 留档；复制实体目录后编译成功。两源 TU 均插桩，系统 libc++ 等预编依赖不插桩；本机监控器实现的独立最小链接不等于真实产品完整构建。

`run.py` / `tsan_run.py` 使用 conda base Python 以每进程 5 秒 / 8 秒上限分别运行启动前 stop、重复 stop、四线程并发 stop 和 callback 内 stop；后两项将 worker 暂停在 fake context 里，校验外部 stop 等待 callback 返回。原版 ASan/UBSan：启动前 0，重复/并发/重入均 `-6`；修复版四项均 0。原版 TSan 在重复 stop 明确报告 `_stopped` 的 data race；部分原版并发运行触发 TSan 本身的线程注册 CHECK，不能据其归因。修复版四项 TSan 均退出 0。`repeat.py` 对修复版并发、重入用例分别使用 ASan/UBSan 和 TSan 各重复 10 次，共 40/40 进程退出 0；日志逐次保存。进程本地 fake context 不代表真实 WtEngine 的线程排空或任意 provider。

后续为区分自 join 与并发 join，fixture 增加 `self_alone`：仅 worker callback 调 stop，主线程在 callback 返回后再 stop/join。重建四个二进制后，旧版 ASan/UBSan 以 join 错误退出 -6，旧版 TSan 报竞争并退出 -6；修复版两种插桩均退出 0。`*-self-alone.log` 保存输出，`hashes.txt` 指向该次最终 fixture；上文四例及 40 次结果运行于增加此分支之前，四条既有用例逻辑未变。`compile-final*` 对应补充重建。
