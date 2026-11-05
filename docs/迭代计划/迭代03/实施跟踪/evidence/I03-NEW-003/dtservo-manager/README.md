# I03-NEW-003：WtDtServo 连接命令状态传播子项

WtDtServo `ParserAdapterMgr` 保留原非虚 `void run()`，增加 `runWithResult()` 聚合所有 parser 的连接命令结果，失败时报告 id，继续遍历以保留原扇出行为，且不释放已有 adapter。`WtDtRunner::start()` 消费聚合结果，新增 C 导出 `are_parser_connect_commands_submitted()`。命令提交并不表示异步连接成功；失败也可能已有其他命令被提交，保留所有者不代表回调已排空。旧 `isInitialized()`/`is_initialized()` 含义已恢复：初始化后连接期间仍为 true。原 C 函数签名未改，新增 C 函数；`WtDtRunner` 布局改变，相关代码必须共同重编。

## 调用方兼容复核

仓库内现有 C 查询消费是测试 `r8_wtdtrunner_test.cpp` 和导出入口 `WtDtServo.cpp`；仓库外动态调用者不可枚举。旧实现先设置 `_is_inited=true` 再调用 `start()`，同步 connect 可重入，原 C API 在该时点返回 true。第一版候选错误地把提交结果并入 `isInitialized()`，使这一时点和失败后返回值改变；保留 `current/` 证据但该候选已撤回。最终源码分离状态，用 fake 的同步 connect 回调验证旧状态保持 true、提交状态在回调内为 false，回调完成后按提交结果变化。

## 定向验证

macOS arm64，调用 `/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python /tmp/wt_i03_servo_compat_build.py`。`compat/wt_i03_servo_compat_build.py` 为脚本副本；`compat/commands.json` 记录每条编译、链接、运行命令、目录及退出码。重新编译 WtDtServo 的 adapter、runner、数据管理器、helper、C 导出、测试和假插件；链接复用 `/tmp/ctpm-i03-build` 中其余预构建静态库与动态库，未验证完整源码树或其余依赖的一致性。Boost bridge 是实体目录，不创建符号链接。

8 个 GTest 各在独立进程运行，8/8 PASS，均退出 0，无 skip：同步 connect 回调查询旧/新状态；C 导出成功及失败；真实 Runner 失败保持 owner 和成功控制；manager 混合失败/成功报告失败并保留两者；manager 正常扇出及 adapter 命令结果控制。输出及 XML 位于 `compat/`，这是增加原子查询之前的候选；最终源码结果见下文。假插件无异步工作线程，不代表真实 SDK 的连接或回调排空。

`red/` 使用较早版本测试及受控源码副本模拟旧版丢弃 manager/runner 返回结果（不是旧源码完整构建）：两个针对 manager 及 Runner 的用例各退出 1，断言定位到误报成功。完整记录和替换源码在 red 子目录。这是行为反例，不是原始历史源码红控，也不是最终接口的直接红控。

首次单独链接 adapter、runner 和测试时，缺少由动态查找提供的 WtDataManager 符号，runner 构造于地址 0 崩溃；随后补充当前 WtDataManager、WtHelper 及 C 导出对象重编，得到上述运行结果。首次构建的崩溃不作为产品缺陷或通过证据。根目录原有 5 项证据来自检查点前候选；`current/` 保留语义调整前的 6 项验证，均不能证明最终源码。

## 仍未闭合

WtCore、WtDtCore manager 和 WtDtPorter 的连接失败传播未覆盖；partial startup 不回滚、runner 重试与并发行为、provider 的 callback drain/生命周期以及真实 CTPMini、其他平台、sanitizer、完整树和整体回归均未验证。新增状态不代表实时 provider 就绪；该子项不能关闭 I03-NEW-003。

## 并发状态查询补充

最终版将新提交状态改为 `std::atomic<bool>`，查询不读取既有非原子 `_is_inited`；每次 start 发命令前置 false，结束后写入聚合结果。`are_parser_connect_commands_submitted()` 可与一次启动并发查询，不扩展现有 initialize/start 并发调用、旧 `is_initialized()` 与初始化并发、析构或 provider 回调生命周期合同。

`atomic/` 是最终树的定向结果：单独编译相关源和 fake 插件后 9/9 PASS，含通过真实 C 导出、屏障阻塞 provider connect、另一个线程读取状态 10000 次的用例；仅在 connect 返回后状态变 true。调用 `/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python /tmp/wt_i03_servo_atomic_build.py`，具体命令、源码与二进制哈希见本目录。其他目标和动态依赖仍复用旧构建，整体树及真实 SDK 未证。macOS 本机已归档的同环境 TSan 控制程序无法运行（见 R41-003/drain），本项未获 TSan 结论；屏障测试不能代替数据竞争检测。`compat/` 是修正原初始化语义但未增加原子变量的旧版证据，不代表最终源码。
