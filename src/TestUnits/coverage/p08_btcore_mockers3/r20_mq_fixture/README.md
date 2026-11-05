# R20 回测通知器 fake MQ 定向检查

这是独立的 CMake/CTest 夹具，不依赖或修改项目的共享测试配置，不启动真实消息服务。它检查三个必需导出分别缺失、可选 publish 缺失、服务创建返回零、完整插件、失败重试、重复初始化，以及正常单次初始化的正控，共 10 项。fake 库按与仓内 `WtMsgQue.h` 一致的签名构建，并核对高位服务 ID、通知长度和销毁记录。

需要 CMake、C++17 编译器、已构建且与当前工具链兼容的 `WTSTools` 和 `WTSUtils` 静态库，以及其所需的 Boost、fmt、spdlog、rapidjson 头文件和链接依赖。必须显式提供两个静态库路径。按实际安装环境调整以下示例中的路径；头文件目录及额外链接项用分号分隔：

```sh
cmake -S src/TestUnits/coverage/p08_btcore_mockers3/r20_mq_fixture \
  -B /tmp/wt-r20-notifier-build \
  -DWT_R20_TOOLS_LIBRARY=/path/to/libWTSTools.a \
  -DWT_R20_UTILS_LIBRARY=/path/to/libWTSUtils.a \
  '-DWT_R20_EXTRA_INCLUDES=/path/to/boost/include;/path/to/fmt/include;/path/to/spdlog/include;/path/to/rapidjson-parent' \
  '-DWT_R20_EXTRA_LIBRARIES=/path/to/boost_filesystem;/path/to/boost_thread;iconv'
cmake --build /tmp/wt-r20-notifier-build
ctest --test-dir /tmp/wt-r20-notifier-build --output-on-failure
```

使用带有编译器覆盖率插桩的静态库时，还需根据工具链提供相应的编译和链接参数。测试在构建目录的隔离工作目录运行，分别将 fake 调用记录写入 `logs/`；每次测试会清空自己的记录。夹具不创建符号链接。本独立入口目前仅为类 Unix 平台准备，实际跨平台行为需要另行验证。

这些测试仅证明受控 fake 模块的加载及调用行为，并不证明真实 MQ 的线程排空、回放停止屏障、模块安全卸载或整个项目回归通过。

2026-11-04（R20_wtbtcore#3，卡9）新增 3 项：`release_destroys_once`（显式 `release()` 恰好一次 destroy、释放后通知与 init 被拒绝、析构不重复 destroy）、`release_before_init`（未初始化即释放后 init 拒绝、无插件调用）、`concurrent_release`（fake `publish_message` 由 `WT_R20_MQ_SLOW_PUBLISH_US` 放慢，4 个线程持续发布期间另一线程 `release()`；断言 destroy 恰好一次且为最后一次插件调用、fake 未记录 destroy 与在途发布重叠或 destroy 之后的发布）。fake 只记录调用，不起真实服务。Runner 级释放顺序见 `q06_btporter/s5_r20_runner_notifier_release_test.cpp`（R160 fake）。
