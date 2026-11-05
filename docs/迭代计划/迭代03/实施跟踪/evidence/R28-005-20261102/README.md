# BUG-R28-005 定向复验与 null 项反例

2026-11-02，本机 macOS arm64。使用检查点 `ccaad010` 的 `src` 和 `dist/common` 实体快照，未修改共享产品或测试文件；产品、库和测试通过隔离 CMake 构建。`focus_snapshot.py` 仅在快照中裁剪 wtcore 测试 TU 至 `n02_exec_mgr_test.cpp`、WtRunner 测试 TU 至 `r28_runner_main_test.cpp`，并令实体 Boost 头优先。这不是完整构建。生成的 232 条 C/C++ 编译命令均将 `/opt/homebrew/Cellar/boost/1.88.0/include` 放在 `/opt/homebrew/include` 之前；快照与构建目录 `find -type l` 未发现链接，见 `symlinks.txt`。`sha256.txt` 中最终 wtcore 测试源码包含附加探针，产品源码来自检查点；当前工作区可能已继续变化。

```sh
mkdir -p /tmp/wt-i03-r28-005-20261102/snapshot
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-r28-005-20261102/snapshot
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python docs/迭代计划/迭代03/实施跟踪/evidence/R28-005-20261102/focus_snapshot.py
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-r28-005-20261102/snapshot/src -B /tmp/wt-i03-r28-005-20261102/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r28-005-20261102/build --target CoverageTests_wtcore CoverageTests_m_WtRunner -j 6
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python docs/迭代计划/迭代03/实施跟踪/evidence/R28-005-20261102/run.py
```

configure、构建和最终运行分别退出 0。5 个 manager 定向与 4 个 runner 配置定向各自独立进程、30 秒上限，**9/9 PASS，0 skip**；详见 `results.json` 和 XML。初次运行未把真实 `libWtDataStorage.dylib` 复制到隔离目录，runner 的合法正控因找不到动态库而失败，原日志与结果保留在 `initial/`；随后从本次隔离构建复制实体模块并重跑通过。不能将最初缺模块视作产品缺陷或抹除该失败。

## 实际 null 数组元素反例

已有用例覆盖 null 配置、null executer 属性和非对象项，**没有覆盖解析前数组中的 null 元素**。本轮将 `null-probes.inc` 附加到隔离快照的测试 TU（未修改仓库测试），重编 `CoverageTests_wtcore` 后在独立目录、每例 30 秒上限运行：

```sh
git show ccaad010:src/TestUnits/coverage/n02_wtcore_engines/n02_exec_mgr_test.cpp > /tmp/wt-i03-r28-005-20261102/snapshot/src/TestUnits/coverage/n02_wtcore_engines/n02_exec_mgr_test.cpp
cat docs/迭代计划/迭代03/实施跟踪/evidence/R28-005-20261102/null-probes.inc >> /tmp/wt-i03-r28-005-20261102/snapshot/src/TestUnits/coverage/n02_wtcore_engines/n02_exec_mgr_test.cpp
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r28-005-20261102/build --target CoverageTests_wtcore -j 6
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python docs/迭代计划/迭代03/实施跟踪/evidence/R28-005-20261102/run_probes.py
```

四个断言受控拒绝的探针均退出 1：JSON、YAML 的 `[有效规则,null]` 被压为一项且 manager 接受；JSON 的 `executer:["exec",null]` 被压为一项并被接受；JSON 规则 `[null]` 被压为空数组且 manager 接受。每个实际断言和 XML 见 `probe-*.log`/`.xml`、`probe-results.json`。探针第一次增补编译因 `get(0)` 重载歧义失败，修为 `get(0u)` 后重编退出 0；失败日志 `probe-build-final.log` 保留。

静态检查 `WTSCfgLoader.cpp` 的 JSON 和 YAML 转换均对**所有数组**的 null 元素执行 `break`，同时对象中的 null 字段也被忽略；该 loader 被 WtRunner、WtDtServo、WtPorter、回测和多个配置/数据调用者共用。改变共享 loader 的 null 表达会影响它们的历史行为，单凭本项不能决定兼容方案。没有修改共享 loader；本条因 null item 仍可绕过而未闭合。

此结果只覆盖本机受控 fixture 和所列快照，未验证全部活动 executor 的 quiesce 内部状态、真实 provider、其他平台、sanitizer 或最终整合树。最终代码继续修复后须重新验证。
