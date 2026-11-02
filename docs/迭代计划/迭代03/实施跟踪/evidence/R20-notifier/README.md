# R20_wtbtcore#3 部分修复：动态加载预检与 ABI

- `build-and-test.sh` 在空 CWD 使用实体 fake dylib，编译当前 EventNotifier.cpp/WtHelper.cpp 和本仓测试代码；链接历史 `/tmp/wt-i03-r58-build` 的 WTSTools、WTSUtils 两个静态库。包含动态 loader、事件通知真实调用，未执行全部产品依赖的当前源码冷构建；命令和原始输出见 `build-test.log`，所用文件与静态库 SHA-256 见 `source-sha256.txt`。
- 10/10 通过：缺 create、destroy 或 register 均无注册/创建；缺可选 publish 仍可初始化且 no-op；create 返回 0 时 init 失败；正常服务/无 publisher 各独立、重复 init 保护，失败后有效插件重试，以及零 ID 后重试。fake 使用声明于 `WtMsgQue.h` 的 WtUInt32 签名、32 位高位 ID 并检查发布长度和销毁 ID。未创建符号链接或真实服务。
- 原始源码在 `baseline`，`red-test.sh` 重编原始实现并做对照：缺 destroy 被错误当成成功并创建（exit 1），缺 register 崩溃（139），零 ID 被错误当成成功并发布（exit 1），重复 init 创建多个服务（exit 1）；缺 create 和缺符号后重试为已有通过控制。`red-positive.sh` 在关闭重复 init 断言时验证原实现完整插件和缺可选 publish 的正向行为（2/2 PASS），避免将原有有效功能当成新修复。输出保存在 red-build-test.log、red-positive.log 及 red/*.stdout。
- 本测试夹具为独立构建，未接入共享 CMake。真实 WtMsgQue、Windows/Linux、长生命周期服务和全仓依赖未测。正常成功时保留原有动态模块引用：尚未设计/证明停止 replay、排空并发调用后安全卸载的合同，未实现 release / runner shutdown 屏障；该项仍不可整体关闭。重入、并发调用、异常路径和模块多实例生命周期也不在证据范围内。

## 仓内可复现入口

`src/TestUnits/coverage/p08_btcore_mockers3/r20_mq_fixture/CMakeLists.txt` 及其 README 现在提供独立 CMake/CTest 构建，不更改共享构建配置。`repo-cmake-cold.sh` 在全新目录用当前源码编译所有 6 个 fake 库和被测通知器；`repo-cmake-cold.log` 记录构建与 10/10 测试通过，`find ... -type l` 输出为空。链接仍复用 r58 的两个静态库，编译器插桩参数为匹配这两个已有库而显式传入。更新后的 `source-sha256.txt` 包含仓内 CMake 和 README；原始 red 对照日志保留不变，red 运行时使用的是增加单独清理测试日志之前的测试源版本。
