# BUG-R18-006 / R18-012：DtCore parser 初始化与配置引用局部复验

2026-11-02，macOS arm64。基于检查点 `ccaad010` 导出 `src` 和 `dist/common` 实体快照；复制当前工作区的 `WTSCfgLoader.cpp/.h` 覆盖快照对应文件。`ParserAdapter.cpp/.h`、两个 loader 文件及既有测试在复验时与工作区逐项同哈希，见 `sha256.txt`；其他检查点后的工作区修改没有全部合入，所以这不是当前完整树的构建。没有修改工作区产品代码、共享 CMake 或既有测试。

隔离快照仅将 `CoverageTests_dtcore` 的测试 TU 限为 `r18_parser_adapter_test.cpp`，增加本机实体 Boost 头优先路径（`focus.patch`、`root-boost.patch`）。为弥补原测试缺少通过模块 `init()` 的成功和失败 cfg 引用断言，在**隔离快照**临时加入 `test.patch`，使用仓库现有的非线程 R130 fake 模块；该测试未接入仓库常规测试目标，后续仍需产品化集成。`symlinks.log` 为空，没有创建符号链接。最终 `compile_commands.json` 中 1139 个含 `/opt/homebrew/include` 的编译命令实体 Boost 路径均在其之前；`build-commands.txt` 留有目标命令。

## 构建

```sh
mkdir -p /tmp/wt-i03-r18-006-012/snapshot /tmp/wt-i03-r18-006-012/plugin
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-r18-006-012/snapshot
cp src/WTSUtils/WTSCfgLoader.cpp src/WTSUtils/WTSCfgLoader.h /tmp/wt-i03-r18-006-012/snapshot/src/WTSUtils/
clang++ -std=c++17 -fPIC -dynamiclib -I /tmp/wt-i03-r18-006-012/snapshot/src -I /opt/homebrew/Cellar/boost/1.88.0/include -I /opt/homebrew/include -undefined dynamic_lookup /tmp/wt-i03-r18-006-012/snapshot/src/TestUnits/fixtures/r130_parser_lifecycle.cpp -o /tmp/wt-i03-r18-006-012/plugin/libR130LifecycleParser.dylib
# 将 focus.patch、root-boost.patch 和 test.patch 对应的改动应用到隔离快照。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-r18-006-012/snapshot/src -B /tmp/wt-i03-r18-006-012/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r18-006-012/build --target CoverageTests_dtcore -j 6
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-r18-006-012/run_tests.py
```

实际快照准备脚本保存在 `setup.py` 和 `add_register.py`；最终构建是 `build-physical3.log`、退出码 0。初轮构建（`build.log`）及测试虽然完成，但发现 Homebrew 链接 Boost 路径优先，不能作为本轮实体依赖证据；前两次修正构建因 include 优先级仍不符合预期而主动中止（`build-physical.log`、`build-physical2.log`，退出 130），不计通过。最终快照已验证优先级，目标重编退出 0。

## 定向结果

六个用例在独立进程、独立目录中分别运行，各上限 45 秒、退出 0、XML 1 test / 0 failures / 0 disabled；最终也在同一进程合组运行，6/6、退出 0，无跳过。命令和退出码见 `results.json`；各日志/XML 在 `isolated/`、合组在 `combined/`。五项现有用例：

- `InitExtReturnsFalseWhenApiInitFails`：外部 fake `init(false)` 拒绝，不订阅、不连接，外部确认安全后 cleanup 一次。
- `InitReleasesRetainedConfigWhenNoApiWasCreated`：空 module 失败后析构使 cfg 引用回到基线。
- `ReleaseReturnsRetainedConfigWhenModuleLoadFails`：缺模块后显式 release 使 cfg 引用回到基线。
- `InitExtSubscribesEveryContractFullCode`：外部 fake 成功时订阅已知两个完整合约码。
- `ReleaseIsIdempotent`：外部 fake 成功初始化后重复 release 不重复清理。

临时附加的 `ModuleConfigReferenceOnFailedAndSuccessfulInit` 通过真实本地 fake 动态模块的 `init()` 验证失败/成功均先持有 cfg、人工设定 fake 静默后释放至基线，provider release 和 delete 各一次；失败时不订阅、不 connect。测试明确观察到两种情况都是 **registerSpi 先于 init、包括 init 返回 false 时已注册一次**，不能据此声称失败不注册。fake 静默由测试显式标记，不代表 SDK 在失败或 release 后自动排空；成功对象在真实异步回调中的引用有效性、失败回调、owner 隔离、真实 provider、Windows/Linux、sanitizer 与最终整合树均未验证。两条原任务仍未闭合，整体回归未运行。
