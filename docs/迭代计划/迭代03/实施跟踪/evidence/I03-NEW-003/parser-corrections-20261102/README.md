# 解析器启动状态回归修正（2026-11-02）

本项是 I03-NEW-003 的局部修复，不关闭该问题。WtRunner 原 main 将 parser 命令状态作为唯一退出条件：parser 成功后 engine 抛异常，原逻辑仍可返回 0。现保留原 parser 查询语义，并独立记录被接受的 run 调用是否正常返回；main 要求两者均为真，异步模式的正常返回只反映启动函数返回，不证明 provider 就绪。配置失败、同步 callback 请求 shutdown 和异常时不提前销毁已有 owner。

WtDtPorter 配置文件或内容无法加载时阻止首次快照启动，故不把空快照误报告为提交成功；以后成功读取并验证配置可以重试。原 schema 校验失败的不可重试行为不变。默认未调用 initialize 的旧行为保留；已开始运行后的动态 parser 不计入首次快照。

## 红绿证据

- `red-run0.log`：把原 main 使用的 parser-only 退出判据封装成临时 `runSucceeded()` 红控，真实 runner fake engine 在同步及异步启动中各断言失败（exit 1）；它是受控的行为红控，不是历史源码原样提供该方法。
- `red-run1.log`：缺失配置后旧版首次空快照返回 true，测试 exit 1；`red-run2.log`：可解析且 parser 为空的正控 exit 0。正控 writer 模块特意不存在，只证明配置与 parser 快照语义，不证明 writer 初始化或服务就绪。
- 最终独立进程定向：WtRunner 5/5（含真实 runner fake engine 异常、启动中重入 shutdown、parser 失败、同步/异步正控）；WtDtPorter 6/6（缺配置、保留现有 parser、空 parser、成功重试、部分提交和动态注册边界）。命令及退出码见 `green-runner-final-results.json`、`green-dt-final-results.json` 和各 `final-*` 日志。
- `main-results.json` 用当前 main 控制流、stub runner 和四种 run/parser 布尔组合验证进程码 1/1/1/0；这是入口逻辑的受控探针，不是完整真实产品端到端运行。

## 构建与局限

通过 `/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python /tmp/wt-i03-parser-correction/build.py` 和对应 `dtporter.py` 在隔离目录重编相关翻译单元，使用 `/tmp/ctpm-i03-build` 原 Ninja 命令，实体 Boost 头 `cov_boost_include/boost` 与 Cellar 路径；其余静态及动态依赖复用预构建对象，不是干净整仓构建。最终编译 argv 和退出码见 `commands.json`、`dt-commands.json`，源与二进制哈希见 `final-hashes.sha256`；快照时间不能代表整个并行工作区。未创建符号链接，未连接真实 provider 或交易。

未验证 SDK 连接/回调排空、状态监控和真实 writer、并发 initialize/start、跨平台、sanitizer 或整合回归；WtRunner 运行中即时通知和失败后的交易暂停仍缺失。用例只验证对应本地语义。
