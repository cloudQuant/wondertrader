# SEL factory 定向证据（2026-11-02）

`baseline/` 保存本轮更改前的工作区 SEL manager 源和匹配头；`initial-baseline/` 是此前隔离实现前保存的旧工作区快照，均非当前完整仓库快照。独立新目录使用实体 Boost/fmt/spdlog 头构建 manager、logger stub 和 fake 动态库。编译包装器 `/private/tmp/wt-i03-hft-factory/clang-asan` 使用 Homebrew LLVM clang++、`-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -include stdlib.h`；实际命令和逐项退出码见 `command-and-results.log`，版本见 `compiler-version.txt`，源文件测试前后哈希一致，未创建符号链接。命令：

```sh
WT_SEL_BASELINE_DIR=/private/tmp/wt-sel-quarantine/baseline \
WT_SEL_OWNER_BASELINE_DIR=/private/tmp/wt-i03-sel-owner-baseline \
CXX=/private/tmp/wt-i03-hft-factory/clang-asan \
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 \
src/TestUnits/fixtures/sel_factory_quarantine/run.sh /private/tmp/wt-i03-sel-owner-final8-20261102
```

撤回 factory 自动 remover 候选后，当前 18/18 场景退出 0，覆盖 creator/getName 异常、隔离后健康加载、空/空名重扫、同名冲突、并发/重入、真实长目录完整加载、无导出和外持 wrapper；长路径正控加载真实假模块，日志只显示有界目录，未截断文件访问。初始隔离前基线的长无效目录红控退出 134，ASan 报告 logger 格式化时 heap-buffer-overflow；初始基线 creator/getName 异常红控退出 134，empty、null、重复扫描、缺少 remover 的四个红控退出 1，健康正控退出 0。分别检查 `logs/` 内具体失败，不能将信号或一般非零一概归因为 ASan 错误。测试使用局部插桩，LSan 被关闭，不证明无泄漏。

无预加载句柄的当前测试验证两个创建重载、null wrapper 和 manager 销毁后外持策略正常使用及策略恰好释放。工厂不随之释放：额外的 `owner-final-removal` 目标测试退出 1，记录所需生命周期闭环仍未实现，并非 UAF 反例。成功模块保留；marker 最后的 `unload` 发生在进程退出时，不代表主动卸载成功。

本轮曾试行在最后 wrapper 后调用 factory remover，fake 测试结果在 `candidate-run.log` 和 `candidate-logs/`，但接口缺少第三方 factory 后台任务排空合同，该候选可能提前销毁仍在使用的对象，已从生产源码撤回，旧候选结果不代表当前源码。此前 HFT 的现有 owner 同样会在最后 wrapper 后调用 remover，真实后台任务排空仍需复核，不能依据 fake 通过认定安全。`early-run.log` 记录一次错误预期：期待旧版分配故障失败但其控制成功，未计为红控；`early-initial-baseline.log` 记录错误要求初始版健康模块不得卸载导致的失败，健康对照已修正。

`ISelStrategyFact` 只定义 factory 的创建、枚举、策略创建与删除，没有后台排空或自线程析构合同；仓内 WtSelStraFact 为空析构不能证明第三方行为。异常工厂、本地同名冲突实例及 loader 引用仍为隔离而保留；跨 DSO 加载、并发 strategy-map、真实 C API/logger、平台及整树回归未验证。本证据不关闭 NEW-033。
