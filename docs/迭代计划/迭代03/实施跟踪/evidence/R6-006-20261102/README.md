# BUG-R6-006 本机配置失败子项

2026-11-02，macOS arm64、Apple clang 17。`ccaad010` 的 `src` 实体快照；WtPorter 和两份相关测试相对于检查点无后续工作区差异，其他依赖可能已变化。未修改产品或共享构建配置。

```sh
mkdir -p /tmp/wt-i03-r6-006/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r6-006/snapshot
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python prepare.py
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-r6-006/snapshot/src -B /tmp/wt-i03-r6-006/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r6-006/build --target CoverageTests_porter -j 6
sh run.sh
```

`prepare.py` 修改隔离快照 CMake，确保实体 Boost 优先并将 porter 测试源裁剪为 r46/r69 两个 TU；不是完整 porter 测试目标。配置和构建均退出 0，日志随附。生成的 1111 条编译命令均含实体 Boost 且优先于 Homebrew include；快照、构建与运行目录符号链接扫描为空。源码及二进制摘要见 `sha256.txt`。

`run.sh` 将每项放入独立进程和目录，分别限时 60 秒；三个退出码均为 0，XML 显示每项 1/1、0 skip：

- `R46PorterConfig.MissingEnvMakesConfigFail`：缺 env 时 config 失败、engine 为空，run 不启动。
- `R69PorterRunnerConfig.ExplicitMissingDataStoreModuleFailsConfigAndLeavesRunnerNotReady`：显式不可用 store 时 config 失败，查询安全且执行状态 NOT_READY。
- `R69PorterRunnerConfig.MissingExecuterFileAndEmptyDataStillReturnTrue`：空 data / no-store 的现有可选模式对照成功，查询无 reader 返回空。

空 data 不等于缺失 data；原验收缺 data 须失败与现有可选模式仍有合同冲突。真实插件可用性、真实 provider、显式 release 与 logger 生命周期、其他平台、sanitizer 和最终整合树均未验证，不关闭整项。
