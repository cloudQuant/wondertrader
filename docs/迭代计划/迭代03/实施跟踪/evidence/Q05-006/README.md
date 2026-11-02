# BUG-Q05-006 重采样会话模板定向复验

从 `/tmp/wt-i02-fresh-20261101-v8-build` 的 `CoverageTests_dtcore` Ninja 命令提取参数，使用当前 `WtDtHelper.cpp` 和 `q05_resample_bars_test.cpp` 独立重编并替换这两个目标对象重链，其余对象和库复用 v8。完整执行 argv 在 `commands.json`，退出 0 的编译/链接输出在 `build-0.log` 至 `build-2.log`；`.d` 显示产品 TU 引用实体 `cov_boost_include/boost/config.hpp`。目标和工作目录均未创建符号链接。nm 在 `product-symbols.txt` 确认目标可执行文件定义 `resample_bars`。五个相关源码/头的构建前后和复核时哈希一致，见 `hashes.json`；其他旧对象没有全部与当前源码比对。

两个指定单例分别以独立进程、隔离 cwd/TMPDIR、30 秒超时运行，均 1/1 PASS、exit 0、无 skip（具体 argv、输出和 XML 在 results.json 与 run0..1）：

- `Q05ResampleTest.BadSessionTemplateRejected`：覆盖 malformed JSON、根/offset/auction/sections 缺失与错误类型，并有 auction 省略和 null 的合法结构对照；
- `Q05ResampleTest.MinuteResampleReportsConsistentCounts`：合法模板下本机临时分钟 K 线重采样，返回值、计数回调和结果回调数量一致。

源依赖文件中这两个新编译 TU 均未包含 `ModuleHelper.hpp`；其它旧对象可能包含旧版本头，不能将混合链接视为当前完整工作树构建。本轮未使用 sanitizer，未覆盖全部时区或边界、其他平台、真实数据提供方、完整依赖构建及整体回归。未修改仓库文件、未新增符号链接。
