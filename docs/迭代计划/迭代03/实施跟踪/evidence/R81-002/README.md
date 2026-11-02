# BUG-R81-002 当前源码定向复验

在 `/tmp/wt-i03-r81-002` 独立输出，以 v8 构建的 Ninja 命令为模板，仅重编当前工作区的 `CtaStraBaseCtx.cpp` 与 `r81_cta_gap_test.cpp`，再与只读旧 v8 对象/库重链。`commands.json` 为各步实际参数，`build-0.log` 至 `build-2.log` 为成功退出日志；相关编译使用实体 `/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include/boost` 并优先于系统路径。`hashes.json` 保存编译前后四个关键源码和二进制 SHA，源码无漂移；未证明其他依赖为当前源码。

`results.json` 为三项逐进程、每项 30 秒上限的实际运行命令与退出码；run0 至 run2 中 XML 各 1/1 PASS、零 disabled：

1. `ConditionRecoveryRejectsInvalidEnumsAndKeepsValidRecord`：真实 JSON 中 field/alg/action 不合法项各自拒绝、记录诊断、不触发；合法条目逐字段恢复且仅触发一次。
2. `ConditionAlgorithmsActionsAndNoMatchCode`：正常条件动作及不匹配控制。
3. `ScheduleTwentiethEmitPersistsUserDataAndConditions`：正常持久化条件记录控制。

在运行及构建目录未发现符号链接；未进行 ASan/UBSan，也未重跑旧版红控。macOS arm64 本机定向结果，其他 v8 对象/静态和动态库均被复用，未全量插桩；真实交易接口、Windows 和最终工作树整体回归未验证。
