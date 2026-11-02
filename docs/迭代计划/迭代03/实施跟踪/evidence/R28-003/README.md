# BUG-R28-003 WtRunner 配置失败定向复验

对当前 `WtRunner/WtRunner.cpp`、`WtRunner/main.cpp`、`WTSUtils/WTSCfgLoader.cpp` 与 `r28_runner_main_test.cpp` 各独立重编，并最小链接 runner 产品/测试与当前 loader 对象；四个 TU 各使用 `-Werror`，实体 Boost bridge 优先。命令数组与输出分别见 `commands.json`、`build-0.log` 至 `build-4.log`，均退出 0；其余静态/动态依赖复用 v8，不是完整当前树构建。`source-hashes.json` 记录相关源码、头文件及二进制 hash；`Share/ModuleHelper.hpp` 亦纳入构建前后及运行后对照，未见漂移（50f18e5b9efadbebeef1cb9c6ff46f5d2488ee68ad2ef207d843c0297c78688f）。

按 `results.json` 的四个 gtest 单例命令分别运行，每例拥有独立 cwd、TMPDIR 和 30 秒超时；均退出 0，XML 各 1/1 PASS，无 skip。两个成功用例的 `generated/portfolio/trades.csv` 和 `closes.csv` 确在各自临时 cwd 下生成；没有写入仓库。真实 `libWtDataStorage.dylib` 从旧 v8 普通文件复制入各临时 cwd，源与副本 hash 见 `plugin-hashes.txt`，未创建符号链接。

损坏 YAML 的实际文件是 `run/unparsable_yaml_returns_false_without_throwing/r28wt_broken_cfg.yaml`，其 `Logs/r28wt_root_2026-11-01.log` 包含相对于该 cwd 的准确路径 `./r28wt_broken_cfg.yaml` 和 YAML 错误摘要；记录同时含配置加载失败日志。损坏 JSON 用例没有调用 `runner->init` 初始化日志，本次 output.log 没有任何错误日志，因此未能动态验证 JSON 路径诊断；源代码 `WtRunner::config` 的空配置分支将 filename 写入日志，但不能代替运行证据。

边界：没有注入非 YAML 类型的异常，不能据此证明不会吞其他异常；没有测 Windows、真实供应商或完整工作树、整体回归。历史相关依赖来自 v8，对其构建输入不作当前源码一致的保证。

## 补充：初始化文件日志后的非法 JSON 路径探针

为补足原 JSON 单例未初始化日志的缺口，在 `/tmp/wt-i03-r28-003/json_log_probe.cpp` 中创建最小独立程序，先创建并初始化文件日志，再对真实写入的损坏 JSON 调用当前 WtRunner::config。该程序使用之前重编的当前 runner.o、loader.o，其他库复用原 v8；编译和链接命令为 `json-probe-commands.json`，源码、对象和二进制哈希见 `json-probe-sha256.txt`。不修改原测试及原四项结果。

在独立 cwd 执行：

```sh
(cd /tmp/wt-i03-r28-003/json-log-run && TMPDIR=/tmp/wt-i03-r28-003/json-log-run/ LLVM_PROFILE_FILE=/tmp/wt-i03-r28-003/json-log-run/profile-%p.profraw perl -e 'alarm 20; exec @ARGV' -- /tmp/wt-i03-r28-003/json_log_probe)
```

`json-log-run/output.log` 记录 `result=false`、实际输入绝对路径 `/private/tmp/wt-i03-r28-003/json-log-run/probe-invalid-config.json`；`exit.txt` 为 0；`Logs/probe_root_2026-11-01.log` 的 `Loading config file ... failed` 包含完全相同的绝对路径，且该文件实际存在。源码构建和测试后再次哈希核对无漂移；日志目录无符号链接。此为本机初始化日志场景，未验证未初始化日志时的可观察诊断。
