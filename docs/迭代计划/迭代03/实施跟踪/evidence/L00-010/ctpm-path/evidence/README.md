# L00-010 CTPMini 模块目录子项（本机证据）

工作区先前已有两个产品路径 guard 和测试夹具，本轮接续检查并完成验证；仅对 `run.py` 的 GTest include 设置改为 `-isystem`，保持产品/测试 TU 的 `-Werror`，更新夹具 README。系统重启已清除旧临时构建，故独立重新配置：

```
/opt/homebrew/bin/cmake -S src -B /tmp/ctpm-i03-build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DWT_WARNINGS_AS_ERRORS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS='-I/opt/homebrew/Cellar/boost/1.88.0/include'
/opt/homebrew/bin/ninja -C /tmp/ctpm-i03-build -j 2 CoverageTests_t_ParserCTPMini CoverageTests_t_TraderCTPMini
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/ctpm_empty_module_path/run.py /tmp/ctpm-i03-build /tmp/ctpm-i03-current
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/ctpm_empty_module_path/run.py /tmp/ctpm-i03-build /tmp/ctpm-i03-red /tmp/ctpm-i03-evidence/baseline
```

配置和构建退出 0，编译了两目标的全部依赖，但没有运行无关测试。构建中产品源码同时还可能受其他项目人员修改，未证明其他所有依赖构建时均是同一完整快照。对当前源码和测试分别重编、用 spy 拦截目标 TU 的 `dladdr`/`dlopen`；两侧空目录相对目标都返回失败、不调用 loader，之后调用 `connect` 安全返回；正常目录都经真实系统 loader 打开实体 fake 动态库；空目录加显式绝对路径都保留原有路径加载尝试（此场景只验证路径和调用，不证明实际成功加载）。共 6/6。产品 TU 无 spy 宏单独编译通过。R105 Parser 和 R61 Trader fake API 原控制各 1/1。

另在隔离目录运行当前 CMake 目标的 `T3_006_TraderCTPMini.successful_fake_module_creates_and_releases_stub_api`：真实 fake 动态模块初始化、connect、disconnect 1/1 PASS，日志和 XML 位于 `good-control/`。命令：
```
(cd /tmp/ctpm-i03-evidence/good-control && LLVM_PROFILE_FILE=/tmp/ctpm-i03-evidence/good-control/profile.profraw perl -e 'alarm 30; exec @ARGV' -- /tmp/ctpm-i03-build/build_x64/Debug/bin/TestUnits/CoverageTests_t_TraderCTPMini --gtest_filter=T3_006_TraderCTPMini.successful_fake_module_creates_and_releases_stub_api --gtest_output=xml:/tmp/ctpm-i03-evidence/good-control/result.xml)
```

红控由 `make_baseline.py` 从当前源码仅移除两处路径 guard 构造（其 UTF-8 BOM/行尾随文本读写被规范化，语义不变）；两项空目录相对目标均被 spy 捕获 loader 调用并以预期的 42 退出，4 个非空目录/显式绝对路径正控通过。差异 `parser-own.diff`、`trader-own.diff` 包含该文件的原有混合行尾格式差异。`/tmp/ctpm-i03-current/commands.json` 和 `/tmp/ctpm-i03-red/commands.json` 保存每个 argv、cwd、退出值和日志；`hashes.json` 和本目录 `source-sha256.txt` 记录当前输入。构建、当前和红控输出树 `find -type l` 为空；未创建符号链接。

初次 fixture 编译 Trader GTest 控制因该第三方 GTest 头被普通 `-I` 引入而触发 `-Werror,-Wsign-compare`；失败原日志和原命令保存在 `current-first-test.log`、`current-first-commands.json`，将头改为与 CMake 一致的 `-isystem` 后重跑通过，没有关闭源码警告。没有真实 SDK、Windows/Linux、ASan 或整体回归。Parser 的既有 missing-library/missing-creator 分支可能空指针调用；路径 fixture 在实际加载的正控中有意在 loader 之后停止，因此不能声称该问题已修。真实 SDK callback 生命周期、其他加载入口和实际绝对路径加载成功仍未验。
