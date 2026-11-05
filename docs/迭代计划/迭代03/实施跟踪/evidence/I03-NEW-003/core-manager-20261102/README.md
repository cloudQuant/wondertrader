# I03-NEW-003：WtCore / WtDtPorter 启动命令提交状态子项

## 合同和改动

- WtCore `ParserAdapterMgr` 保留 `void run()`，新增非虚 `runWithResult()`：逐个调用并聚合是否发出 connect 命令，false 不阻止其余调用、不移除 owner；沿用原异常向上传播，不证明回调已排空。旧入口委托新入口，不改变签名或类数据布局。同步调用期间不得并发或重入修改 manager 容器；本子项不扩展该合同。
- WtRunner 保留 `void run(bool)` 与原有启动顺序，新增原子查询 `parserCommandsSubmitted()`，开始及同步连接期间为 false，完成后反映聚合值；false 时保留现有运行 owner、继续既有启动流程并输出诊断，未宣称可安全回滚。仅表示命令发送，不表示连接完成或行情就绪。类布局改变，相关目标须共同重编；既有可执行调用端目前没有读取该查询。
- WtDtPorter 的原快照启动路径已独立处理单个 parser，新增原子查询和独立 C 导出 `initial_parser_connect_commands_submitted()`，仅表示首次启动快照内全部命令提交，不包括随后动态创建的 parser。false 包括尚未启动、正在连接或失败；true 不代表连接、就绪或动态 parser 成功。失败仍记录并继续旧流程、保留 owner，不代表完成回滚或 callback drain。旧 C 签名不变；C++ runner 类布局改变。

## 定向证据

macOS arm64，使用 `/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python` 分别执行 `build.py`、`dtporter.py`、`verify.py`。确切编译、链接及测试命令和退出码见 `green-commands.json`、`dt-commands.json` 和 `verify-commands.json`。重新编译相关的 parser adapter、两个 runner、两个测试 TU、WtDtPorter C 导出及其本地辅助文件等，**其他依赖复用 `/tmp/ctpm-i03-build` 的既有库，不能代表整树干净构建**。Boost bridge 为实体目录，未创建符号链接。

6/6 定向通过：WtCore adapter/manager 2，WtRunner 成功与部分失败 2，WtDtPorter 部分失败与 C 查询（含启动后动态创建失败不改变初始快照状态）2。均由进程内 fake API 完成；同步 connect 内查询 false，部分失败后两 owner 仍由 manager 持有，成功 parser 仍收到命令，旧 manager `void run()` 也验证扇出。测试日志和 XML 随附，命令使用 30 秒上限。新增 WtDtPorter 测试最初通过隔离编译执行；现已接入独立 CMake 覆盖率目标，具体见补充记录。

`red.py` 与 `red-dt.py` 在隔离副本中将聚合结果强制设为 true，模拟旧行为忽略失败；两次构建链接成功，manager、WtRunner、WtDtPorter 对应 3 个部分失败断言均失败，见 `red-*-run.log` 与命令记录。这是受控行为红控，并非声称历史版本原样构建。`final-before.sha256` 与 `final-after.sha256` 相同，代表最终测试前后所列相关源码没有变化。`baseline/` 为操作前拷贝，WtRunner 和其测试当时已有未提交改动，未覆盖；不构成整个工作区基线。

## 未闭合

未验证实时 provider 的实际连接/后续断线、外部 SDK callback-drain、跨平台、完整最终树及整体回归，未运行 sanitizer。WtPorter 和 WtExecMon 的 runner 仍使用旧 void manager 入口；WtRunner 的可执行 main 仅在同步循环返回后消费该查询并设置退出码，不提供即时运行状态。WtDtPorter 动态 parser 的失败不在初始快照查询内。没有定义部分启动失败的安全回滚、重试和对交易准入的业务策略；调用方不能将 true 视为可交易的就绪信号。I03-NEW-003 仍为阻塞。

## 追加：常规构建目标与退出码

新增 `CoverageTests_i03_dtporter_start` 目标，直接链接 WtDtCore、WtDtPorter 生产 TU 及其依赖；从通用 `CoverageTests` 排除该测试 TU，避免混入另一个同名但布局不同的 WtCore `ParserAdapter`。这是仓库现有覆盖率构建风格中的可执行目标，项目未给这些目标注册 CTest 用例。源树其他 CMake 脏改动保留，`baseline/coverage-CMakeLists.txt` 是本子项修改前副本。以下隔离目录未生成符号链接：

```
/opt/homebrew/bin/cmake -S src -B /tmp/wt-i03-start-cmake -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWT_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
/opt/homebrew/bin/cmake --build /tmp/wt-i03-start-cmake --target CoverageTests_i03_dtporter_start --parallel 4
```

全新构建该目标和 WtDtCore/WTSTools/WTSUtils 依赖成功，两个定向用例独立进程各 1/1 PASS；`wt-i03-start-configure.log`、`wt-i03-start-build.log`、`wt-i03-start-rebuild.log` 与 `normal-target-results.json`、`normal-*.xml` 保存命令结果。只在 macOS 实测，其他平台未构建。

`WtRunner/main.cpp` 仅在原先 `run(false)` 返回之后，根据 `parserCommandsSubmitted()` 返回 0 或 1；不会因为部分失败立即返回并触发 owner 析构。`R28ParserCommands.synchronous_run_reports_only_after_loop_returns` 用 fake API 控制循环退出并在 runner 析构前核对成功/失败状态及 parser owner；全部三个 R28 定向用例 3/3 PASS（`runner-sync.*`）。`main-probe/` 使用与当前 main 相同的控制流、替换两个 include 并以 stub runner 模拟 `run(false)` 返回，对应 false/true 的进程退出码分别为 1/0；这只验证入口分支，不是完整可执行程序、真实 provider 或产品整链运行。`rebuild-commands.json` 包含相关 TU 重编与链接记录，仍有预构建库复用。

此退出码要等现有循环结束，不是运行中的即时失败通知，也没有阻止交易器和引擎继续启动。查询只描述初始命令提交，不应作 readiness 或活跃健康标志；如何在保留 provider owner 的前提下暂停交易、通知宿主、恢复或回滚仍未决定。WtDtPorter 启动后的动态注册成功/失败不更新初始快照状态，动态创建接口各自返回结果；外部 host 若只调用旧 void C 接口且不查询新接口，仍无法得知首次启动失败。WtPorter/WtExecMon 仍待接入。`current-sources.sha256` 为追加后源码快照；前述 `final-*.sha256` 是追加前测试快照。
