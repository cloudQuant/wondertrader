# R103-002：查询游标污染局部修复

实施前和实施后源码均来自当前已有未提交工作树，而非干净 HEAD。产品实现只删除 `get_last_tick` 对共享 `_query_cursor == 0` 的提前拒绝；查询仍按原有时间定位。新增用例先查询首笔以前的空历史切片，再在当前精确 tick 时间查询最新 tick，验证历史查询不会使后续查询永久返回空。

本机使用 `build.sh` 在 `/tmp/wt-i03-r103-cursor` 单独编译前后版本的 `HisDataReplayer.cpp` 和同一最终版测试，使用 LLVM ASan/UBSan；链接 `/tmp/ctpm-i03-build` 中已有静态库，其他代码不保证与当前树一致，且预编库未完全插桩。构建 exit 0；相应哈希见 `sha256.txt`。此前两个尝试因编译参数和 dyld 查找库问题没有成功运行，不计测试结果。

在该临时目录分别运行两个版本，设定 `DYLD_LIBRARY_PATH=/usr/local/lib:/opt/homebrew/lib`、`ASAN_OPTIONS=detect_leaks=1:detect_container_overflow=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，过滤器 `R103LastTickBoundaryTest.*`：实施前 4/5、exit 1，新增测试因当前 tick 为空失败；修改后 5/5、exit 0，未见 sanitizer 报告。详细输出及原始 XML 在本目录；这不是完整工程构建或全量回归。

间隔/尾后应返回空还是截至当前的最后一笔仍未决定；原有尾后测试仅验证现行行为，不将其作为已确认产品合同。同时间戳多笔的逐回调可见性尚未验证；本条保持未闭合。
