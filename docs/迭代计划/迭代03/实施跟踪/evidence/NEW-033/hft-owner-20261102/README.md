# HFT factory/wrapper 独立定向复核（2026-11-02）

当前 `ccaad010` 工作树中的 HFT manager 共享 owner 候选在本次工作开始前已存在；本次未改产品源码。基线 `baseline/` 是此前共享 owner 实施前的工作区源文件和匹配头，不是 Git HEAD。原始基线 hash 与本次源码前后 hash 见同目录 `*.sha256`；测试运行期间列出的源码哈希一致。此前独立 ASan 红控同样记录在 `/private/tmp/wt-hft-review/run.log`，但该临时文件不是本证据的依赖。

使用实体 Boost/fmt/spdlog 头目录，隔离新目录编译 manager、logger stub、测试和假动态模块；不创建符号链接、不连接真实交易。命令中的 wrapper 实际调用 Homebrew LLVM clang++，同时开启 `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`：

```sh
WT_HFT_BASELINE_DIR=/private/tmp/wt-i03-hft-owner/baseline \
WT_HFT_BASELINE_KIND=owner CXX=/private/tmp/wt-i03-hft-factory/clang-asan \
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 \
src/TestUnits/fixtures/hft_factory_quarantine/run.sh /private/tmp/wt-i03-hft-independent-20261102
```

完整命令和退出码见 `command-and-results.log`，分场景输出在 `logs/`。当前版本 25/25 场景退出 0；两个前一版本 wrapper 生命周期红控均退出 134，ASan 报告 `HftStraWrapper::~HftStraWrapper` 读取已释放的 factory；前一版本健康正控退出 0。新 `owner-no-preload` 不预先 `dlopen` 插件，走 `factory.strategy` 重载；manager 析构后策略仍可调用，然后按 strategy、factory 顺序各释放一次。marker 的最终 unload 来自进程退出，不代表主动卸载已验证。另一个 owner 场景覆盖两个外持实例、weak_ptr，以及最后一个 wrapper 释放时工厂精确销毁。测试证据只覆盖 fake 插件、本机路径、指定 TU 和配置；禁用 LSan，不声称无泄漏。

成功模块的 loader 引用仍驻留至进程退出；无 remover、失败或冲突工厂也按现有逻辑保留资源。此设计避免无排空合同情况下提前卸载，但不是资源释放证明。跨 DSO registry、插件长期活动与 remover 重入、任意 strategy-map 并发、真实插件和 SDK、Windows/Linux、完整仓库构建及回归未验证；本组证据不关闭 NEW-033 整项。
