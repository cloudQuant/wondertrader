# BUG-R5-001 UDP 订阅绑定与重试定向复验

从 `/tmp/wt-i02-fresh-20261101-v8-build` 的 `CoverageTests_dtcore` Ninja 命令提取编译和链接参数，将当前工作区的 `UDPCaster.cpp`、`r154_udp_caster_start_test.cpp` 和 `r164_udp_caster_active_test.cpp` 独立重编至 `/tmp/wt-i03-r5-focused`，链接时替换对应三个目标对象；其余对象和库复用 v8。具体 argv 在 `commands.json`，编译/链接输出 `build-0.log` 至 `build-3.log`。Boost 使用实体 `cov_boost_include` 路径，三个 .d 中均记录相关 Boost 头；本输出目录未创建符号链接。`product-symbols.txt` 为二进制中当前 UDPCaster::init/start 的定义；产品、测试及辅助头六文件的编译前后 SHA 和可执行文件 SHA 在 `hashes.json`，运行后再次核对文件未变。此结果不能证明其他依赖与当前完整工作树一致。

两个单例分别在独立 cwd 和 TMPDIR、上限 30 秒进程中运行，命令与 XML 属性见 `results.json`：

- `R154UdpCasterTest.SubscriptionBindFailureReturnsFalseAndCanRetry`：1/1 PASS，exit 0；先占用内核分配的动态端口，验证 init 失败，释放端口后同实例重试成功，再以本机 loopback 短请求验证接收回包；
- `R164UdpCasterActiveTest.BportFallbackBindsSubscribePort`：1/1 PASS，exit 0；使用本机动态端口验证 bport 回退绑定和 loopback 请求/响应。

两例 2/2 PASS、无跳过。此证据仅证明本机端口占用错误和回环路径，未验证真实网络/供应商环境、Windows 或 Linux、并发关闭和 sanitizer；测试夹具显式 stop 后为规避既有生命周期问题保留 caster 对象，不能推断析构安全。旧对象/库未全量重建或插桩，未运行整体回归。本轮没有修改工作树或创建符号链接。
