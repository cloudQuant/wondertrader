# BUG-L00-005 混合归档 / ActionPolicyMgr 实测记录

本目录记录只读链接检查和一次已启用、无交易/无真实插件的定向测试。没有运行 CMake/Ninja 构建，没有修改产品、测试或构建文件。构建目标由主代理先前统一刷新。

## 结论边界

- `CoverageTests_m_WtUftRunner` 确实把 `libWtUftCore.a` 与 `libWtCore.a` 放在链接行，同时链接 `libWtExecMon.dylib` 与 `libWtPorter.dylib`。最终 Mach-O 的 ActionPolicyMgr 符号为 `U`，`dyld_info -imports` 指明实际导入来自 `libWtExecMon`。对当前构建产物全量检查 dylib 后，只有 `libWtExecMon` 和 `libWtPorter` 导出这些相同的 `T` 符号；`libWtExecMon` 的 Ninja 链接规则明确包含 `libWtCore.a`。因此当前 UFT runner 测试目标的 ActionPolicyMgr 调用没有抽取并绑定 `WtUftCore/ActionPolicyMgr.cpp.o`，而是绑定到以 WtCore 构建的动态实现。
- 安全过滤器 `r95_uft_config.string_basefiles_and_array_modules_return_true` 1/1 PASS。该 fixture 在自己的临时 cwd 创建本地 YAML；缺失的 `bspolicy` 让 `_act_policy.init` 走配置加载失败返回分支，且 parser/trader module 是不存在名称、在 `dlopen` 前失败。新 profile 对 `libWtExecMon.dylib` 的 `src/WtCore/ActionPolicyMgr.cpp` 显示 ctor/dtor/init 分支各命中 1；目标 executable 对 `src/WtUftCore/ActionPolicyMgr.cpp` 无 coverage map。原始 stdout、XML、profraw、profdata、精确过滤器、returncode、超时和目标 SHA-256 见同目录 `mrunner-1793244481375674000.*`。
- 不能把同一 profile 加到 `libWtUftCore.a` 后得到的行计数当成 UftCore 执行证据：这个 archive 查询将相同 mangled name/hash 的 WtCore 运行计数映到 UftCore 的同名函数，并报告 `538 functions have mismatched data`。对具体的动态库或 executable 对象做 source mapping 才有归属意义。
- 主 `CoverageTests` 目标的链接闭包含 WtCore/WtDtCore/WtBtCore，但不含 WtUftCore；其最终 executable 的 `nm` 与 dyld imports 都没有 ActionPolicyMgr 符号。故它不是本 duplicate pair 的混合目标，不能据此说明 WtCore/UftCore 同名实现混合安全。其他不同同名类未在此结论范围内。
- WtCore 与 WtUftCore 两份 ActionPolicyMgr 当前可执行语句相同，差异仅在注释/排版。本证据确认的是测试目标的家族归属错误和潜在回归盲区，不声称已经观察到两份实现的行为差异，也不外推为产品 WtUftRunner 运行错误。产品 `src/WtUftRunner/CMakeLists.txt` 只链接 WtUftCore/WTSTools/WTSUtils 和系统库。

## 建议的最小 CMake 隔离

只为 `CoverageTests_m_WtUftRunner` 使用与产品 `WtUftRunner` 同源的最小链接依赖，并额外加入 `GTest::gtest_main` 和线程目标：`WtUftCore WTSTools WTSUtils dl pthread boost_filesystem boost_thread ${STDCXXFS} ${ICONV_LIBRARY}`。不要给这个 test target 链入本构建中导出 ActionPolicyMgr 的 `WtExecMon` / `WtPorter`，也不要继承把所有 plugin dylib 塞进该进程的全局 `${WT_COV_SHARED_LIBS}`。只改变测试目标闭包；保留 WtExecMon/WtPorter 各自 targets 及其 WtCore 链接，其他测试目标继续可验证它们，因此无需产品 ABI、类名或库结构变更。若某个新增用例需要 ExecMon/Porter，应另设专属目标/进程，避免与 UftCore 的相同强符号共置。

此问题可直接按仓库当前 runner CMake 链接合同修复，不需业务 owner 决定。重建隔离后的测试目标时可加 macOS link map，确认 `libWtUftCore.a(ActionPolicyMgr.cpp.o)` 被抽取、`libWtCore.a(ActionPolicyMgr.cpp.o)` 未抽取；另要求最终 executable 的 `nm` 对 ActionPolicyMgr 为本地定义而非 `U`、`dyld_info` 无该符号导入，并用同一安全过滤器的 profile 确认 executable 映射 `src/WtUftCore/ActionPolicyMgr.cpp` 且没有 WtCore 源码命中。

## 可复核命令

```sh
xcrun nm -A -C build-cov/build_x64/Debug/libs/libWtCore.a build-cov/build_x64/Debug/libs/libWtUftCore.a build-cov/build_x64/Debug/bin/WtPorter/libWtExecMon.dylib build-cov/build_x64/Debug/bin/WtPorter/libWtPorter.dylib build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_m_WtUftRunner | rg 'ActionPolicyMgr::'
xcrun dyld_info -imports build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_m_WtUftRunner | rg 'ActionPolicyMgr'
xcrun llvm-cov show --instr-profile=docs/迭代计划/迭代02/复核185/evidence/01_share/bug-l00-005-mixed-link/mrunner-1793244481375674000.profdata --show-line-counts build-cov/build_x64/Debug/bin/WtPorter/libWtExecMon.dylib src/WtCore/ActionPolicyMgr.cpp
```

运行命令在 `mrunner-1793244481375674000.json` 中保存；固定过滤器为 `r95_uft_config.string_basefiles_and_array_modules_return_true`，15 秒超时，实际耗时 0.271 秒，GTest XML 为 1 test / 0 failures。
