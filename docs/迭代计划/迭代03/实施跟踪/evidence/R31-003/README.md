# BUG-R31-003 动态插件定向验证

使用独立 `/tmp/wt-i03-r31/build` 输出，基于 `/tmp/wt-i03-udp/source/src` 快照重编 WtExecuterFactory.cpp、v2_exec_plugin_factory_test.cpp 及 fake_exec_plugin.cpp 的 null_factory（mode 27）和 valid（mode 0）两个真实动态模块。测试 TU 的两个插件路径直接指向新构建的实体 dylib，测试通过 copy_file 放入隔离目录，不创建符号链接。4 个 TU 的依赖文件显示 Boost 取自实体 `/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include`。每条编译、动态模块及测试可执行文件链接命令与 cwd 见 `commands.json`。

每例在独立目录和进程、有界 20 秒执行，结果各 exit 0、XML 1/1 PASS、0 disabled：

- `V2ExecuterFactoryTest.NullFactoryIsUnloadedWithoutBeingPublished`：空 factory 未发布、模块 unload 的日志顺序。
- `V2ExecuterFactoryTest.NullFactoryDoesNotPreventValidPluginLoad`：空 factory 不阻碍真实 valid dylib 创建单元。
- `V2ExecuterFactoryTest.RepeatedLoadSkipsNullAndDuplicateFactoriesSafely`：重复扫描后 factory 和 unit 的创建/销毁及加载/卸载计数。
- `V2ExecuterFactoryTest.WrapperKeepsModuleLoadedAndDestroysUnitBeforeFactory`：有效插件对象持有模块、对象与工厂释放顺序对照。

源快照与当前工作区在执行前后对 WtExecuterFactory.cpp/.h、IExecPluginV2.h、ExecuteDefs.h、两个 fixture/test TU 的 SHA 一致，见 `hashes-before.json` 和 `hashes-after.json`；二进制与两个新模块哈希见 `binary-hashes.json`，`factory-symbols.txt` 显示 factory 入口定义于生成的执行文件。其他 WtCore 和工具依赖静态库沿用旧 v8 构建，不证明当前完整树一致；没有 sanitizer、Windows/Linux、真实第三方插件或外部回调并发生命周期验收，不能据此宣称整项生产验收通过。未运行整体回归。
