# Porter replay status V1

`get_backtest_status_v1` 是真实 Porter 动态库新增的 C 导出：调用方将 `WtReplayStatusV1.struct_size` 设为可用字节数、`abi_version` 设为 1。大小至少为 V1，较大的尾部不写；返回 0 表示已复制快照，-1 表示空指针、大小不足或版本不支持，-2 表示查询抛出异常。无效参数不写入输出。输出为调用方自己的值，不含借用字符串。结构体定义和状态数值见 `src/Includes/ReplayStatusV1.h`。

`EVENT_BACKTEST_END` 是终止通知；回调中查询得到当前 generation 的暂定状态和 flags=0。结束 hook 和 notifier 返回后该代次快照设置 `WT_REPLAY_STATUS_FINAL`，才可以作为稳定结果；结束回调本身抛异常可使先前的 SUCCEEDED 变为 FAILED。FINAL 仅覆盖 Replayer 的结束阶段，不能推断 Runner 或 provider/plugin 可以销毁，也不保证 Runner 的外层启动准入已解除。

从仓库根目录复跑（目录必须不存在）：

```sh
bash src/TestUnits/fixtures/i03_porter_replay_status_20261103/run.sh /tmp/wt-i03-porter-status-review-new
```

脚本在独立目录使用 `cmake --build ... --target WtBtPorter -j2` 构建真实动态库及核心依赖，验证真正导出符号，并以新进程分别测试参数/IDLE、同步成功及连续两代、异步成功、真实 CTA 无费成交失败、回放停止、结束回调抛异常和初始化回调抛异常。每个正常构建用例均通过公开导出函数运行，终止回调内和返回后均调用导出查询。

线程创建失败另用只在临时复制的 `WtBtRunner.cpp` 中插入一个 `std::bad_alloc` 的隔离动态库验证；其余生产 Runner、Porter 及依赖为真实实现，查询仍走真实 C 导出。独立 Python 文件以 exclusive create 生成注入源码，验证恰好一处可逆插入并记录源哈希；不依赖预先存在的注入文件。该测试验证未发生 tick、回调内 FAILED/INTERNAL 且 flags=0、结束后 FINAL。

`evidence/test.log` 为七个正常构建场景、`evidence/launch-test.log` 为注入场景、`evidence/source-check.log` 为注入源码检查；各场景和脚本均退出 0。`evidence/source-before.sha256` 与 `source-after.sha256` 一致，记录关键源文件与测试输入；并非所有传递依赖的哈希。`evidence/build.log`、`configure.log` 和 `export.log` 记录独立构建和符号。原始四个 Porter 文件的本轮前副本和哈希在 `baseline/`。最初注入试跑可能复用了旧临时文件，未计作证据；本结果来自全新构建目录。

只验证本机 macOS arm64 的上述功能路径。未验证完整回归、真实行情/provider、跨平台、完整 sanitizer 或外部调用方语言绑定。无效非空地址不能由 C 接口安全验证，调用方仍须提供可访问的结构体内存。
