# BUG-L00-010 POSIX 模块路径局部修复（非整项验收）

仓库：`/Users/yunjinqi/Documents/new_projects/wondertrader`，本机 macOS arm64。修改前从脏工作树实体拷贝 `src/Share/ModuleHelper.hpp` 和 `StrUtil.hpp` 到 `baseline/Share`；`own-changes.diff` 仅记录相对该拷贝的本次改动，既有 Windows 分支原样保留。

`getInstPath` 现在检查 `dladdr` 返回值及 `dli_fname`，失败返回空字符串；两个 POSIX 缓存使用 C++ 局部 static 一次性初始化。失败也会被永久缓存：不再在相同进程和翻译单元中自动重试。`getBinDir` 的成功路径保留尾部 `/`，无分隔符时为空。

## 定向红控

本次夹具 mock 的 failure 情形显式令 `dli_fname=nullptr` 并返回 0；null 情形返回成功但 `dli_fname=nullptr`，用于稳定触发旧代码的危险输入。执行：

```
c++ -std=c++17 -pthread -I /tmp/wt-i03-l00-posix/baseline src/TestUnits/fixtures/module_helper_posix/fake_dladdr_test.cpp -o /tmp/wt-i03-l00-posix/baseline/fake_dladdr_test
for mode in failure null empty valid no_separator concurrent; do
  perl -e 'alarm 15; exec @ARGV' -- /tmp/wt-i03-l00-posix/baseline/fake_dladdr_test "$mode" > "/tmp/wt-i03-l00-posix/red-$mode.log" 2>&1
  test_status=$?; printf '%s exit=%s\n' "$mode" "$test_status"
done
```

结果：failure=139、null=139（均 SIGSEGV）；empty=1（非空缓存条件导致重复查询）；valid=0、no_separator=0；concurrent=1（24 个受控并发访问均进入 dladdr，原子计数 24），原始输出为 `red-*.log`。受控输入证明未检查失败及并发初始化问题，不代表真实系统一定以同样方式填充错误的 `Dl_info`。

## 当前源码绿控

```
src/TestUnits/fixtures/module_helper_posix/run.sh /tmp/wt-i03-l00-posix/green-final2 > /tmp/wt-i03-l00-posix/green-final2.log 2>&1
```

退出 0：失败、null、空串、有效绝对路径、无分隔符路径、24 线程首次调用 6/6，加真实可执行文件及独立实体动态库地址和尾部目录检查 1/1；脚本内含完整编译及运行命令。`source-sha256.txt` 固定测试后输入的哈希；`git -c core.whitespace=cr-at-eol diff --check -- src/Share/ModuleHelper.hpp` 通过。`symlinks.log` 为空：本次临时输出树无符号链接，也未创建符号链接。

限制：仅本机 macOS arm64，未运行 TSan、Linux、Windows、整个项目或整体回归。失败后缓存为空，而 `src/ParserCTPOpt/ParserCTPOpt.cpp:122` 当前将空目录直接拼模块名，仍有加载相对路径的风险；未修改调用方，整个 BUG-L00-010 仍需逐一审计全部调用者及平台验收。真实 provider、跨模块卸载/重载、多 TU 和异常资源故障不由本证据证明。
