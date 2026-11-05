# R10-005 定向复核（2026-11-03）

以先前 R10-005-20261102 的构建和运行脚本为基础，仅替换临时工作目录；脚本和构建日志在本目录。当前工作区的 EventNotifier.cpp、WtHelper.cpp、两个测试 TU 和三个 fake MQ 模块被重新编译；链接的 WTSTools 和 WTSUtils 静态库复用 /tmp/wt-i03-r41-drain/libs 的预编构建。Boost include 指向 Cellar 实体目录；没有创建符号链接，临时输出目录 find -type l 无结果，不断言所有外部依赖路径无符号链接。

4 个测试分别独立运行，全部各 1 test、0 failure、0 skip、exit 0；见 run.log 与 runs/ 的原始日志和 XML。检查指定产品源、测试源、fixture 和两个预编库前后哈希均一致（before.sha256、input-check.log 和 input-check-after.log）；该列表不是完整头文件/依赖闭包，不能保证完整工作树无并行变化。artifact.sha256 固定执行文件和 fake 模块。

验证范围为缺注册导出、缺创建导出、受控 fake 插件 ABI/单事件发布，以及阻塞发布后订单引用销毁。原验收的完整插件全部通知主题尚未由这 4 项覆盖；真实 provider、空闲后的 worker、关闭排空、其他平台、sanitizer 和整合回归未验证，不据此变更主台账或声称最终验收。
