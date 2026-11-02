# BUG-R15-001 HFT 订单标签局部修复（非整项验收）

范围：`HftStraBaseCtx.h/.cpp` 和 `r15_hft_stra_context.cpp`。原始脏工作树三文件副本与哈希在 `baseline/`、`baseline.sha256`；本次相对副本的改动为 `own-*.diff`。并未修改实际订单发送或终态策略。

- 将默认零容量的 circular_buffer 替换为按 localid 精确匹配的无固定容量 `std::map<uint32_t, std::string>`；相同 localid 的后次登记替换当前值，缺失 ID 的查询及删除不会作用于邻居。长期保存无界，ID 重用与迟到回报并未解决。
- 审计发现 `getOrderTag` 会经 on_trade/do_set_position 写入私有 `DetailInfo::_usertag[32]`。改为 std::string、对标量显式零初始化，且平仓日志使用 `.c_str()`；该私有明细仅用于 HftStraBaseCtx 内存中的 PositionMap/vector，未发现二进制持久化或对外传递。已用 31、32、256 字节测试开仓、反手及平仓日志的完整标签，不代表其他组件接受任意长度标签。
- 初次 ASan/UBSan 回调测试发现 `_ud_modified` 读取未初始化 bool，初始化为 false；原始失败在 `pre-initfix-asan/`。构造时 `_trader` 也未初始化，且多处直接解引用；本次未改该独立问题。Apple 自带 ASan 的空程序 smoke 15 秒 SIGALRM（退出 142）；后续使用 LLVM 22，未宣称前者通过。

## 编译和正控

所有命令从仓库根目录执行，必要的系统 nanomsg 实体文件复制成 `/private/tmp/wt-i03-r15-tags/libs/libnanomsg.6.dylib`（不是符号链接）。本机 macOS arm64。

```
/private/tmp/wt-i03-r15-tags/build-normal.sh > /private/tmp/wt-i03-r15-tags/final-normal-build.log 2>&1
/private/tmp/wt-i03-r15-tags/run-focused.sh /private/tmp/wt-i03-r15-tags/final-tests final
/private/tmp/wt-i03-r15-tags/build-asan.sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 /private/tmp/wt-i03-r15-tags/run-focused.sh /private/tmp/wt-i03-r15-tags/asan-tests asan-final
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /private/tmp/wt-i03-r15-tags/verify-results.py
```

命令、编译选项和链接参数详见脚本；每个用例分别独立工作目录、30 秒上限，只运行 7 个 R15 定向用例。`final-results.log`、`asan-final-results.log` 各 7/7 退出 0；`xml-summary.log` 校验每个 XML 为 1 test、0 failure/error/disabled，原始输出/XML 在 `run/final-*`、`run/asan-final-*`。覆盖默认登记、精确删除/不存在邻居、乱序/重复、2048 个订单、长标签、终态回报后当前保留行为以及 open→reverse→close 的标签日志。回调测试没有发送实际订单：登记是测试直接驱动，不能证明从真实 buy/sell 到同步回报的标签注册链。

编译当前 BaseCtx、HftStraContext、WtHftEngine 和测试 4 个 TU，其余静态对象、库取自 `/private/tmp/wt-r14-resume/build` 的较早隔离快照；测试对象排在旧归档之前。普通 4 TU 为 Apple clang 17，ASan/UBSan 4 TU 为 LLVM 22 并指定 Apple SDK libc++ 头；旧依赖未插桩。ASan 关闭 LSan，没有关闭容器检查，也未做 TSan。选定源码/库哈希 `final-before.sha256` 与 `final-after.sha256` 相同，产物在 `artifact.sha256`；并不证明所有传递依赖与当前树一致。

## 反例和保留失败

1. 原始 header/产品来自 R14 旧快照（其 BaseCtx h/cpp 与 `baseline/` 哈希一致），新测试用例 `order_tag_default_capacity_preserves_registered_tag` 退出 1，登记后实际返回空，见 `run/baseline-default/`。其他原始行为的相关反例 `baseline-results.log`；其中长标签用例进程终止，未凭非插桩结果断言根因。首次二进制运行缺 rpath 导致 dyld 无法加载 nanomsg；通过实体文件及为自有测试二进制添加 rpath 后取得上面的红控，早期 dyld 输出未另存。
2. 为单独暴露原 lower_bound 错误，`make-index-red.py` 从原 header 在临时源码副本中仅将容量设为 16 并将 `_ud_modified` 设 false（见 `red-index-header.diff`）；`build-red-index.sh`、`run-index-red.sh` 运行两个定向用例均 exit 1：缺失 ID 3 误读 ID 5，删除 ID 2 又误删 ID 5；乱序插入也误匹配。不是原始零容量产品的运行结果。
3. 为单独检测明细固定数组风险，`make-red.py` 在当前源码临时副本中仅恢复原 `char[32]` 明细与无界复制（`red-detail-*.diff`），保留新的索引及已初始化 bool；相同 ASan+UBSan 运行开仓→反手→平仓测试在 32 字节路径报告 `heap-buffer-overflow`，栈到 `log_close` 对非终止明细标签的读取，exit 134，`run/red-asan-detail/output.log`。该受控版本并非完整原始工作树。
4. 初始 ASan 组合中的两个回调分别报 `load of value ... not a valid value for type bool`，与 `_ud_modified` 未初始化对应；同配置修复后通过。失败日志不可由后来通过结果抹除。

`symlinks.log` 空，没有创建符号链接。完整当前树构建、真实 API、Windows/Linux、完整回归、并发访问、回调前登记、重复 ID 的订单身份、订单终态和迟到成交顺序及有界回收均未验证；map 本身不提供同步，返回的 `const char*` 遇同 ID 更新或删除会失效，不能声称跨并发修改安全。条目应保持阻塞。
