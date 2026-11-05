# BUG-R28-005：Runner 路由 null 预检和 sidecar 快照

2026-11-02，本机 macOS arm64。`WtRunner` 读取主配置一次，在创建 `WTSVariant` 之前读取同一份字节；对于 CTA/SEL，在开始修改运行态前用同一内容检查 JSON/YAML 的 `routers` 数组及其中 `executer` 数组的 null 元素。执行器 sidecar 同样只读一次，预检通过后的 variant 保留至实际加载，避免文件变化导致预检与使用的内容不同。通用 `WTSCfgLoader` 未修改；HFT 不消费 routers，维持旧行为。键的比较采用与 legacy loader 一致的 C 字符串语义，包含嵌入 NUL 的键别名。对象字段 null 仍沿用 legacy 当缺失处理，不改变整个配置解析合同。

旧源码（`ccaad010`）与最终测试编译后，inline/sidecar 两个聚合红控均失败，进程 exit 1，见 `red.log` / `red.xml`；模拟 sidecar 在预检后、使用前被日志回调删除的单例旧代码失败，exit 1，见 `red-snapshot.log` / `red-snapshot.xml`。中间第一版 inline 红控由于最小配置先在无关步骤失败而出现假绿，已经用完整可启动 fixture 改正，`red.log` 为最终测试的旧代码结果。

最终源码的相关定向共 8/8，退出 0，XML 无 skip：`green.xml` 6 项覆盖 JSON/YAML inline/sidecar 的 18 种 null 正反边界、NUL 键别名、保留已有路由、有效 JSON 配置、合法 string/array 路由、合法 sidecar 和 HFT 不消费 router 正控；`diagnostics.log` 保存出错路径与数组索引。`green-snapshot.xml` 独立 1/1 利用同步日志回调在 preflight 后删除 sidecar，验证仍使用已保留的合法配置；`green-alias.xml` 独立 1/1 验证有效嵌入 NUL alias 保持 legacy 的键语义。注意两个聚合测试是 18 个输入场景，并非 18 个 GTest case；分别独立执行快照测试以便控制 logger 单例状态。

构建以 `/tmp/wt-i03-r28-005-20261102/snapshot`（`ccaad010` src/dist 实体快照）复制到 `/tmp/wt-r28-null/snapshot`，覆盖本轮 `WtRunner.cpp` 和测试 TU；仅快照内裁剪测试目标和实体 Boost 优先，差异 `root-cmake.patch` / `coverage-cmake.patch`。未改共享 CMake，快照和构建目录 `find -type l` 无输出。构建命令：

```sh
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-r28-null/snapshot/src -B /tmp/wt-r28-null/build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
/opt/homebrew/bin/cmake --build /tmp/wt-r28-null/build --target CoverageTests_m_WtRunner -j 6
```

构建重试保留：`final-build.log` 因新增测试缺 WTSLogger include 失败，`final-build2.log`、`final-build3.log` 修正并退出 0。`red-build3.log` 用 git HEAD 的旧 `WtRunner.cpp` 和相同最终测试重编。当前工作区源码、测试 TU、最终二进制及本地 WtDataStorage fixture 的 SHA-256 在 `sha256.txt`。各测试在不同独立 cwd 复制实体 WtDataStorage 库，90 秒 alarm；例如：

```sh
LLVM_PROFILE_FILE=/tmp/wt-r28-null/final-group-v3/prof-%p.profraw perl -e 'alarm 90; exec @ARGV' -- /tmp/wt-r28-null/final-binary-v3 '--gtest_filter=r28_wtrunner_config.router_null_preflight_rejects_inline_json_and_yaml_without_mutation:r28_wtrunner_config.router_null_preflight_rejects_sidecar_json_and_yaml_without_mutation:r28_wtrunner_config.routers_section_reaches_load_router_rules:r28_wtrunner_config.executers_side_file_with_inactive_array_loads_zero:r28_wtrunner_config.valid_json_config_succeeds:r28_wtrunner_config.hft_ignores_unused_router_nulls' --gtest_output=xml:/tmp/wt-r28-null/final-group-v3/result.xml
```

另外分别运行 `router_sidecar_uses_preflight_snapshot_after_file_removed` 和 `router_nul_alias_with_valid_executer_keeps_legacy_key_semantics`，每个过滤器只含一个测试，结果归档如上。没有运行无关测试或整体回归。本机相关目标是快照隔离构建，未证明当前全树、Windows、其他编码平台、活跃 executor 的 quiesce 或 provider 生命周期。sidecar 在后续配置阶段的其他文件不在本项一次读取保证内；文件自身读取期间被外部并发重写，也未证明得到原子快照。

## 编码兼容复修（以上 8/8 对应旧候选，不代表当前树）

独立复核指出 `load_from_file` 与 `load_from_content` 的历史编码语义不同：前者转换后即使为空也不回退，后者转换为空会使用原文；将二者替换可能改变接受行为。新增非虚 `WTSCfgLoader::load_from_normalized_content` 静态接口，内部直接调用原 JSON/YAML 解析器而不做编码转换；现有两个入口保持原样。WtRunner 对单次读取的字节完全按原 `load_from_file` 分支转换一次，该规范化结果同时用于新解析入口与 null 预检。YAML 预检使用 `c_str()`，与 legacy YAML 解析器同样按嵌入原始 NUL 截断；JSON 同样沿用 C 字符串解析边界。

`encoding-green.xml` 是当前源码 10/10，其中包括 UTF-8 和 GB2312 的 JSON/YAML 与原 `load_from_file` 配置值比较、首字节非法造成 Linux/macOS 转换为空时拒绝的负控、已规范化入口不进行额外转换的直接测试，以及原始 NUL 截断一致性；其余覆盖本项 null、正常配置和 HFT 行为。`encoding-green-snapshot.xml` 与 `encoding-green-alias.xml` 独立各 1/1，合计 **12/12，0 skip，所有进程 exit 0**。每个进程限时 90 秒。过滤器分别为：

```text
r28_wtrunner_config.router_null_preflight_rejects_inline_json_and_yaml_without_mutation:r28_wtrunner_config.router_null_preflight_rejects_sidecar_json_and_yaml_without_mutation:r28_wtrunner_config.routers_section_reaches_load_router_rules:r28_wtrunner_config.executers_side_file_with_inactive_array_loads_zero:r28_wtrunner_config.valid_json_config_succeeds:r28_wtrunner_config.hft_ignores_unused_router_nulls:r28_wtrunner_config.normalized_loader_does_not_apply_content_encoding_fallback:r28_wtrunner_config.encoded_files_preserve_legacy_loader_normalization:r28_wtrunner_config.invalid_encoded_file_remains_rejected_by_legacy_and_runner:r28_wtrunner_config.embedded_nul_file_keeps_legacy_parser_prefix_semantics
r28_wtrunner_config.router_sidecar_uses_preflight_snapshot_after_file_removed
r28_wtrunner_config.router_nul_alias_with_valid_executer_keeps_legacy_key_semantics
```

在上述同一隔离源码快照增量复制当前 `WtRunner.cpp`、`WTSCfgLoader.cpp/.h` 和测试 TU 后执行：

```sh
/opt/homebrew/bin/cmake --build /tmp/wt-r28-null/build --target CoverageTests_m_WtRunner WtRunner WtDtServo -j 6
```

执行器程序 `WtRunner`、共享库 `libWtDtServo.dylib` 和定向测试目标均成功链接；由 `normalized_dynamic_probe.cpp` 编译的独立程序动态链接 `libWtDtServo.dylib` 后调用新接口，运行 exit 0：

```sh
/usr/bin/c++ -std=c++17 -isystem /tmp/wt-r28-null/snapshot/src /tmp/wt-r28-null/normalized_dynamic_probe.cpp -L/tmp/wt-r28-null/build/build_x64/Debug/bin/WtDtPorter -lWtDtServo -Wl,-rpath,/tmp/wt-r28-null/build/build_x64/Debug/bin/WtDtPorter -o /tmp/wt-r28-null/normalized_dynamic_probe
/tmp/wt-r28-null/normalized_dynamic_probe
```

期间 `encoding-test-final-build.log` 因测试中的临时 `ChartoUTF8` 直接构造 `std::string` 不合法而失败，改为与生产相同的赋值形式后 `encoding-test-final-build2.log` 成功；失败日志保留。当前源码与产物哈希在 `encoding-sha256.txt`。本机动态链接仅证明 macOS 目标和所列调用；Windows/MSVC、系统实际代码页、多种非 UTF-8 编码、读取期间并发修改和最终树整合仍未验证。以上定向测试不替代整体回归。
