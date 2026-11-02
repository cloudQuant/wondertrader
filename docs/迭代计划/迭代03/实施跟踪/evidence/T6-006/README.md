# BUG-T6-006 当前源码定向验证

独立快照 `/tmp/wt-i03-udp/source/src`，对 ParserUDP.cpp、t6_parserudp_reachable_test.cpp 和 udp_handle_write_lock_test.cpp 分别以 LLVM 22 加 `-O1 -fsanitize=address,undefined -fno-omit-frame-pointer` 重编，具体完整命令位于 `commands.json`。优先包含 v8 构建的实体 cov_boost_include，链接其他 v8 动态及静态库，库未经 sanitizer 全量插桩；`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`，`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。禁用了 leak 检测。

逐独立进程运行以下四个相关单例，20 秒超时，全部 exit 0、0 skips，XML/日志见各目录：

- `t6_parserudp_packets.short_and_unknown_frames_are_ignored`：短头、恰好类型头、各数据类型短一字节、未知类型。
- `t6_parserudp_packets.complete_packets_dispatch_aligned_payload_fields`：短帧/无终止字符串/完整行情、订阅返回、逐笔三类数据，断言分发字段。
- `ParserUDPReceiveTest.EmptyBroadcastDatagramRearmsBeforeTheNextValidTick`：本地 loopback 发送空 UDP 数据报并重臂后接收合法 tick，测试最后 shutdown。
- `ParserUDPShutdownTest.ActiveUdpDisconnectEmitsOneCloseAndReconnectStartsNewEventEpoch`：真实本机 UDP socket worker 连接、断开与重连，最后 release；`shutdown/exit.txt` 记录 exit 0。

编译首次失败因 LLVM22/fmt 缺显式 stdlib include，见 `build/compile-t6-initial-failure.log`；二次链接命令包含 Ninja 结尾 shell 记号失败、第三次链接器不支持 `-no_warn_duplicate_libraries` 失败，分别见 `build/link-initial-failure.log` 和 `build/link-second-failure.log`；最终移除不兼容链接选项后成功，最终 `build/link.log` 的三条 DWARF 解析警告没有导致退出失败。无其他测试运行。

快照及工作区关键源码哈希见 `hashes-before.json` 与 `hashes-after.json`，这几份文件一致；不是全部源码一致性证明。工具链使用系统现有依赖，未创建任何符号链接。测试未连接真实供应商或行情发送方，没有证明生产关闭/第三方依赖、Windows/Linux 和最终全树；packet 测试只覆盖所列帧，loopback 是本机受控输入。详细对象与二进制 hash、动态库依赖见 `binary-sha256.txt`、`dynamic-dependencies.txt`。
