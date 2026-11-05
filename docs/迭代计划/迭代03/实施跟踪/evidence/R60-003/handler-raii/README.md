# R60-003：未执行请求引用子项

`TraderMocker` 的 order/cancel handler 在投递前 retain 输入。改为 handler 持有带 release deleter 的 shared_ptr，处理完成或未执行 handler 被销毁时归还引用；共享指针构造或投递异常亦不再因已经 retain 而遗失该引用。API 接口与停止准入不变。停止并不立即清空队列：排队引用仍会保留到 handler 执行或 io_context 析构。

新增 `R60MockerTest.unexecuted_order_requests_release_references_on_destruction`：实例未连接时接受请求，调用 release 后销毁，观察两种输入引用从 2 返回 1。旧产品当前测试红控 `red.log`/`red.xml`：两者均为 2，exit 1；修复版 `green-final3.log`/`green-final3.xml` 6/6、exit 0，含正常执行、关闭后拒绝、停止时准入、端口释放和未知撤单对照。中间曾因脚本通过 conda run 的标准输入执行而未实际修改源码，运行 5/6；它不是修复后验证。最终先以脚本文件修改并重编后取得上述结果。

编译命令与退出码见 `commands.json`：产品和测试翻译单元在 `/tmp/wt-i03-r60-raii` 单独重编，链接复用 `/tmp/ctpm-i03-build` 的既有库；Boost bridge 检查无符号链接。运行命令：

```sh
LLVM_PROFILE_FILE=/tmp/wt-i03-r60-raii/green3.profraw perl -e 'alarm 40; exec @ARGV' -- /tmp/wt-i03-r60-raii/focused '--gtest_filter=R60MockerTest.unexecuted_order_requests_release_references_on_destruction:R60MockerTest.ready_requests_remain_deferred_and_run_after_connect:R60MockerTest.stopped_requests_are_rejected_without_retaining:R60MockerTest.stopping_closes_request_admission_before_join:R60MockerTest.release_and_destructor_free_bound_udp_port:R60MockerTest.cancel_unknown_order_before_any_insert_reports_error' --gtest_output=xml:/tmp/wt-i03-r60-raii/green-final3.xml
```

证据仅限本机 fake SPI、所列翻译单元及测试范围；没有 sanitizer、完整树或其他平台验证。活动 worker 停止时队列去留、每个已接受请求的结果通知、账户状态、跨代重连、重复/并发 release、回调内自 join 和 owner 销毁/卸载生命周期仍未解决，不得据此关闭 R60-003。
