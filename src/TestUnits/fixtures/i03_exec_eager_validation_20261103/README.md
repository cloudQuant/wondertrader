# 迭代03 执行器/SEL 策略 eager 校验夹具（2026-11-03）

关联：BUG-R10-001、BUG-P02-002（卡8b=A 全量预校验）、BUG-R134-003、BUG-WSN06-001（SEL 创建/init 失败传播、部分注册回滚、context on_init 生命周期）。核对：BUG-P02-005、BUG-R202-003、BUG-WSN06-004（本夹具不改其语义）。

## 产品改动范围

- `src/WtCore/ExecPolicyConfig.h`（新增，纯读取）：`policy` 必须是对象且含 `default`；每个条目（含商品专属条目）必须是对象，`name` 为字符串且严格为 `<factory>.<unit>`；`clear` 为对象，`includes/excludes` 为非空字符串或非空字符串数组。
- `WtLocalExecuter` / `WtDiffExecuter` / `WtArbiExecuter`：新增静态 `validateConfig()`（只读：对象、scale、groups[Local/Arbi]、clear[Local/Arbi]、policy）；`init()` 在保留配置、建线程池、读取 Diff 快照之前先调用。头文件仅新增静态成员函数，不改对象布局或虚表。
- `WtRunner`：`config()` 纯预检阶段（`_config` 被替换之前）对 CTA/SEL 的执行器全表做校验，SEL 模式另校验 `strategies.sel`；`initExecuters()` 在加载工厂/构造任何执行器之前再校验全表（含表内重复 id、非对象条目、未知 kind；WtRunner 不支持 `arbi`，按未知 kind 拒绝），SEL 模式把执行器和路由规则注册到 SEL 引擎；新增 `initSelStrategies()`：先全部创建，再全部 init，全部成功后才创建并登记 context，任何失败不登记任何 context；SEL 模式不再读取 `strategies.cta`。
- `WtSelEngine::on_init()`：逐个已登记 context 调用 `on_init()`，再发初始化事件（与 CTA 一致）。
- `WtDiffExecuter` 构造函数补 `_config(NULL)`：ASan 分配填充（0xbe）暴露该成员原先未初始化，`getUnit()` 及失败 init 路径依赖其为空（新发现问题，需另立 ID）。

## 复现

从仓库根目录运行，输出目录须不存在：

```sh
WT_I03_NO_WERROR=1 src/TestUnits/fixtures/i03_exec_eager_validation_20261103/run.sh /tmp/<new>-green green
WT_I03_NO_WERROR=1 src/TestUnits/fixtures/i03_exec_eager_validation_20261103/run.sh /tmp/<new>-red red
WT_I03_NO_WERROR=1 src/TestUnits/fixtures/i03_exec_eager_validation_20261103/run.sh /tmp/<new>-asan green asan
```

脚本复制物理快照（拒绝符号链接），`prepare.py` 只在快照内收窄 `CoverageTests_wtcore`（仅本夹具 `exec_eager_test.cpp`）与 `CoverageTests_m_WtRunner`（`r104_runner_executers_test.cpp` 追加 `runner.inc`），并构建假 SEL 工厂 `libI03SelFact.dylib`（`sel_factory.cpp`，只经 dlopen 加载）。`red` 模式用 `baseline/` 中改动前的 6 个产品源恢复快照（哈希见 `baseline.sha256`）。`-j2`，每个测试进程 `perl alarm 120`。

`WT_I03_NO_WERROR=1`：2026-11-03 当时并行工作包中的 `src/WTSTools/WTSDataFactory.cpp`（非本项文件）在 `-Werror` 下出现 unused-parameter 错误，故快照构建关闭 warnings-as-errors；脚本把本项相关文件的告警抽取到 `own_warnings.txt`，本次为 0 行。这不是提交配置。

`asan`：整个快照用 Homebrew LLVM 22.1.8 `-fsanitize=address,undefined -fno-sanitize-recover=undefined` 构建（Homebrew gtest/boost/系统库未插桩），`-include cstdlib` 规避 Homebrew libc++ 与 fmt 的 malloc 可见性问题；`detect_leaks=0`。

## 用例

`I03ExecEagerValidation.*`（5）：Local/Diff/Arbi 分别对 13/11/13 种非法配置（缺 policy、policy 非对象、只有商品条目缺 default、default 非对象/无 name/name 非字符串、name 无分隔符/空工厂/空单元/多段、非 default 条目非法、clear 非对象、clear.includes 含非字符串）返回 false，并检查无副作用：`_config` 为空、未建线程池（配置 poolsize:2）、groups/clear 集合为空、`_unit_map` 为空、配置引用计数未增加、工厂无存活执行单元；合法配置（default+商品条目、仅 default）三类均成功，且 init 不创建单元。

`I03RunnerExecEager.*`（8）：表中间非法 Local / 非法 Diff / 非法 name / 不支持 kind / 表内重复 id / 非对象条目时 `initExecuters` 返回 false 且 CTA、SEL 两个管理器均无登记；合法表（local+diff+dist+inactive）登记 3 个；SEL 模式登记到 SEL 引擎；`config()` 含非法执行器时在预检阶段拒绝，`_config`、`_engine` 仍为空。

`I03RunnerSelStrategies.*`（6）：合法 SEL 配置（同时带一个必然无法创建的 active CTA 条目）`config()` 成功、创建并 init 2 个策略、启动前 on_init 0 次、`_sel_engine.on_init()` 后 2 次；批中 init 返回 false / init 抛异常 / 未知策略名 / 非法 period / 缺 name 均使 `config()` 失败，且 `on_init()` 不触达任何策略；runner 析构后策略删除数等于创建数。

## 结果（本机 macOS，2026-11-03/04）

证据根目录 `/tmp/wt-i03-exec-eager-20261103/`（各目录含 exit_codes.txt、*.log、*.xml、source.sha256、own_warnings.txt）。

- 最终 green（当前工作树全新快照，含 Diff 构造修复及并行工作包的 WTSBaseDataMgr 改动）`green-final/`：exec 5/5（exit 0），runner 15/15（exit 0），本项文件告警 0 行。
- 早期 green `green3/`：exec 5/5；runner 首轮 11/15（失败原因为夹具 env 指向不存在的 fees 文件被既有费用门禁拒绝），`runner.inc` 去掉 fees 路径后增量重编 `runner2.log` 15/15。
- red（`baseline/` 改动前源码）`red2/`：exec 3/5 失败（三类执行器均接受非法 policy 并建池）；`runner2.log` 14/15 失败（仅合法表登记 3 个的正控通过）。红控原因：基线接受非法列表并已登记前项、SEL 模式执行器登记到 CTA 引擎、预检后才校验导致 `_config` 已替换、SEL 模式误读 `strategies.cta` 并因其失败。
- ASan/UBSan `/tmp/wt-i03-exec-eager-20261103-asan/`：首轮 `exec.log` 中 Diff 用例 1 项失败（`_config` 读到 0xbebebebebebebebe，即未初始化成员，作为该构造缺陷的红控）；runner 15/15。补构造初始化后增量重编（`build-difffix.log`），`exec2.log` 5/5、`runner2.log` 15/15，无 ASan/UBSan 报告。前两次尝试（fmt/malloc 可见性、强制 include 破坏编译器探测）为脚本问题，目录保留为 `-attempt1/-attempt2`，不计结果。
- 既有执行器相关套件影响（`impact-green/`、`impact-red/`，完整 wtcore 目标，`-Werror` 关闭）：24 个套件逐套件、3 个中途 abort 的套件逐用例运行，red 与 green 结果完全一致（同一 22 个失败名、同样的 abort 用例），本改动未改变任何既有用例结果；这些既有失败/abort（如 R10ExecuterTest 16 失败、P02LocalExecuterTest 全部 abort、R71 Diff 自动重试失败与 ConcurrentSameCode 用例挂起）均为改动前已存在，不计通过。
- 既有 `r104_wtrunner_config.*`：red 与 green 均为 6 失败/1 通过，首因是既有费用门禁（`absent_fees.json`）；其执行器条目同时缺 `scale`（P02-005 已要求）和/或 `policy`（本项要求），预期与卡8b=A 冲突，需该 lane 负责方按新合同补齐配置；本项未改该文件、未削弱断言。

## 未覆盖与限制

- 工厂/单元是否真实可加载仍在首次建单元时惰性 fail-closed；`WtExecuterFactory` 无只读探测接口（不在本项所有权），未做 eager 存在性检查。
- Diff 快照读取失败仍在 `init()` 中发生（运行时状态而非配置）；runner 仍按"先登记、失败后整体 quiesce"的既有 fail-closed 路径处理，执行器宿主析构要求先 quiesce，因此无法"立即销毁未登记宿主"。
- SEL：失败批中已创建的策略对象由 `SelStrategyMgr` 缓存到 runner 析构（该文件不在本项所有权，无移除接口）；策略自身 init 的外部副作用无通用回滚；`on_init` 异常隔离、重复 `run()`（BUG-R10-004）、启动时持仓向执行器回放、`date/time` 与 `d1/w1/m1` 语义（R13）均未处理。
- `WtPorter/WtRtRunner.cpp`（R134 原 lane，含 arbi 入口、SEL 路由、`initExecuters` 失败泄漏与忽略返回值）、`WtExecMon/WtExecRunner.cpp` 不在本项所有权，未改；执行器层校验会对其生效，但 runner 层全表预检与 SEL 路由未接入。
- trader 缺失/不存在仍只记日志（未裁定）；账户净额门禁（卡5/6）未接入；重复 init 拒绝（卡9c）未在三类执行器实现。
- 仅本机 fake；真实插件、Windows/MSVC、完整回归均未执行（NOT_RUN）。

## 迭代03 最终验收处置（2026-11-05，slice3-result）

- 分类：**STALE_FIXTURE**（已修夹具）。F3 green：exec 与 r104 exit 0，runner exit 1——`I03RunnerSelStrategies.ValidSelConfigCreatesInitializesAndRunsOnInit` 的“合法”配置里 `period: min` 条目沿用 `time: 1500`，被 K27=A（2026-11-04）拒绝：日志 `strategies.sel[1] (i03_sel_b): min period requires an interval of 1..1440 minutes`。K27=A 规定 d/w/m/y 的 `time` 为 HHMM，`min` 周期的 `time` 为分钟间隔 1..1440。
- 修改（`runner.inc`）：`i03SelEntry()` 增加 `time` 参数（默认仍为 `1500`，d 周期条目不变），`min` 条目显式传 `5`（合法间隔）。其他断言未改；`InvalidPeriodIsRejectedInPreflight`（`period: q`）仍覆盖非法周期。
- red 模式：**blocked/not-applicable**。`baseline/` 中改前的 6 个产品源与当前树组合后，`libWtExecMon.dylib`/`libWtPorter.dylib` 链接失败：当前树中不属于基线集合的 `WtExecMon/WtExecRunner.cpp`（`validateExecRunnerEntries`）与 `WtPorter/WtRtRunner.cpp`（`validate_rt_executer_entries`）引用 `WtLocalExecuter/WtDiffExecuter/WtArbiExecuter::validateConfig`，改前源码没有这些静态函数（2026-11-05 重跑 `run-red/build.log` 实测）。红控依赖的基线源码与当前树不再可组合，无法在冻结树上复现，红绿对照以 2026-11-03/04 原始证据为准。
- green-asan：run.sh 的 asan 分支显式传 `-DCMAKE_{EXE,SHARED,MODULE}_LINKER_FLAGS=-fsanitize=…`，覆盖了 CMake 用环境 `LDFLAGS` 初始化链接参数的行为，运行器为 nanomsg 设置的 `LDFLAGS=-Wl,-rpath,/usr/local/lib` 因此丢失，测试二进制启动即 `dyld: Library not loaded: @rpath/libnanomsg.6.dylib`（exit 134，环境问题，非 sanitizer 报告）。已在三处 asan 链接参数后追加调用方 `${LDFLAGS:-}`，不改变 green/red 模式。重跑结果见最终验收证据目录。
