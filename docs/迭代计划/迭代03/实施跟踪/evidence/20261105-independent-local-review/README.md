# 迭代 03 本地定向复核（2026-11-05）

## 范围与结论

按用户要求复跑待验证任务的可用本机定向用例。`summary.json` 将 76 组运行与 82 个基线条目关联，每组记录过滤器、条目、测试名称和结果；另有 4 组独立夹具，其中 3 组关联另外 7 条，见 `fixtures/summary.json`。最近一轮合计 **729 次测试执行、728 通过、1 跳过、0 失败**，含重复执行；另有 3 个非 GTest 监控探针退出码均为 0。其他待验证条目未在本批运行。

通过仅表明对应过滤器在以下源码快照和本机构建上通过；不能外推到完整条目、最终源码树、正式集中回归、真实厂商、Windows 或生产环境。除下述回放状态夹具对部分翻译单元做 ASan/UBSan 外，本批未自行运行其他 sanitizer 或硬断电测试；未覆盖的专项夹具、生产调用链及合同仍以各条台账为准。`R22PorterCapiTest.ConfiguredStartChildChecksStateMonitor` 实际跳过；禁用用例不计通过。各组在独立工作目录和独立进程中运行，同组内用例并非逐例独立进程。

## 快照与构建

- 本机 macOS，仓库 HEAD 见 `HEAD.txt`；HEAD 不包含已有的大量未提交改动。
- 将当时 `src` 实体复制到 `/tmp/wt-i03-independent-20261105-mKLfbHZv/src`，复制后与工作区 `diff -rq` 一致。快照逐文件 SHA-256 在 `snapshot-src.sha256`。测试期间另一个进程继续修改原工作区，`source-diff-final.txt` 是本轮对比时的差异，不保证此后未再变化。
- 额外从工作区复制 `dist/common` 到快照供回测夹具使用，逐文件 SHA-256 在 `snapshot-dist-common.sha256`。该复制晚于 `src` 快照，并非原子冻结全仓库。
- 独立 Ninja Debug 覆盖率构建，默认 `WT_WARNINGS_AS_ERRORS=ON`，`WT_ENABLE_COVERAGE=ON`，Boost 实体 include 目录 `/opt/homebrew/Cellar/boost/1.88.0/include`，配置加 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`。第一次配置缺该参数而失败，随后补参配置成功。`configure.log` 与 `build*.log` 是实际日志。
- 构建了 CoverageTests、wtcore、adstorage、btcore、dtcore、dtservo、uftcore、btporter、porter、exefact、ParserUDP、TraderMocker、ParserShm、ParserXTP、CTPLoader、QuoteFactory、WtUftRunner 等目标，以及 ParserXeleSkt 和 execv2 fake 插件。二进制与部分模块哈希见 `binaries.sha256` 和 `modules.sha256`。
- 曾误在同一个独立构建目录并行启动第二个 Ninja，发现后立即中断（`build-plugins.log`）；该次仅完成 4 个 ParserUDP 专属对象，后在另一构建结束后串行重新执行，`build-plugins-retry.log` 构建成功。不能将中断视为构建通过。

各组以 `summary.json` 中的 `filter` 运行对应构建出的 Google Test 二进制，在独立目录中设置 `LLVM_PROFILE_FILE`，用 `perl -e 'alarm shift; exec @ARGV or exit 127' 120 ...` 限时并生成 XML。完整 stdout 和 XML 分别保存在 `runs/<组名>/run.log`、`result.xml`。覆盖率 profile 本身未归档；本轮不以覆盖率 profile 判断泄漏。

## 首轮失败与复跑

- `date_binding` 首轮因未构建 `ParserXeleSkt` 失败 1/5。独立构建该模块后 `date_binding_retry` 5/5；失败日志和复跑日志都保留。
- `history_tick`、`session_streams`、`freeze_day`、`mocker_factory`、`day_price` 首轮在回测初始化处失败；源码夹具通过 `__FILE__` 寻找快照旁的 `dist/common`，而最初只复制了 `src`。复制基础数据后分别复跑，除 `mocker_factory_retry` 外均通过。
- `mocker_factory_retry` 30/33，3 例缺 `exec_v2_plugins` 假模块；独立构建 `CoverageTests_execv2` 后 `mocker_factory_retry2` 33/33。该两轮失败均保留，不能当作产品缺陷或被悄然覆盖。
- 写端夹具首次以带覆盖率插桩的静态库链接普通构建，缺 `___llvm_profile_runtime`，未运行测试；随后从同一源码快照建立不带覆盖率的独立构建，重新链接并逐例运行 18/18。初次构建失败记录 `fixtures/writer-build.log`，复跑证据 `fixtures/writer-retry-runs/`。
- 数据解析宿主夹具原 `run.sh` 在本机 Bash 下因 `set -u` 和空数组兼容问题在配置前失败。使用其相同的 CMake 参数、独立目录及快照静态依赖手工执行等价构建，三个 GTest 二进制分别为 10/10、10/10、4/4；原脚本的三个监控模式分别退出 0。首次及复跑记录在 `fixtures/`。
- 策略插件 ABI 夹具分别为 live 11/11 和 mocker 5/5；逐例检查了 XML 和退出码，未仅依据外层脚本无条件返回的 0。
- 回放状态夹具重新编译快照中的测试、Replayer 和 BaseDataMgr，局部 ASan/UBSan 16/16；其他静态库未全插桩，`detect_leaks=0`。首次执行在进入测试前因缺 nanomsg 动态库搜索路径退出 134，设置本机 `DYLD_LIBRARY_PATH` 后复跑通过；首次与复跑日志均在 `fixtures/replay-status/`。STOPPED、失败状态区分及完成回调测试均在本次 XML 中。

专项夹具均使用 `/tmp/wt-i03-independent-20261105-mKLfbHZv/` 中的快照，输出放入该目录 `fixtures/` 下新目录。写端使用 `i03_dt_writer_lifecycle_20261104s5/run.sh <snapshot-src> <writer-retry> normal` 并将 `S5_LIBS` 指向 `build-normal/build_x64/Debug/libs`，再运行 `run_cases.sh <writer-retry> <writer-retry-runs> 60`。策略插件使用 `i03_strategy_plugin_abi_20261104s5/run.sh <strategy>`，`WT_ABI_LIBDIR` 指向覆盖率构建的 `build_x64/Debug/libs`。数据解析宿主使用 `cmake -S <snapshot-src>/TestUnits/fixtures/i03_dt_parser_host_20261104s5 -B <parser-retry> -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_BUILD=<snapshot-build> -DWT_BEFORE=`，构建后在独立目录运行 `dtcore_host_cur`、`servo_host_cur`、`dtporter_host_cur`，设置 `I03_DT_HOST_PARSER_DIR=<parser-retry>/parsers` 并分别输出 XML；`monitor_writer_probe` 的三个参数为 `self`、`self-external-join`、`exception`，第二参数均为 `<parser-retry>`。

## 仍需工作

本批仅覆盖所列条目的部分行为。示例：K20 普通写端和 TSan、K36 跨进程发布、K42 各 provider 迁移与真实回报、日期映射所有调用者、交易恢复和并发合同均未由相应用例证明；CTPLoader 的禁用异步末包场景仍未验证。原始清单和滚动台账列出的其他缺口不因本报告被清除。整体回归仍为 `NOT_RUN`；最终树须重新冻结、重建并按条目核对。

## 按本地范围复核并晋升

先前将本批所有条目一律保留“待验证”过于保守：本地通过不要求先完成适用范围外的跨平台或最终整体验收。针对本批测试、原验收与已裁定合同复核后，以下四项改为 **本地验证通过**，并保留各自限制。相关产品和测试源文件的快照/工作区哈希对照见 `promotion-sources.json`；该对照不证明整个构建依赖图或最终树一致。

- `BUG-I02-NEW-102`：`replay_public` 覆盖回放器未知合约的品种查询及无规则代码返回空串，符合 K38 BT-A/BT-C；其他输入、平台及整合回归未验。
- `BUG-I02-NEW-116`：`shm_subscription` 3/3 覆盖普通及多段代码订阅/退订与相同 exchange/rawcode；真实 publisher、跨平台和整合回归未验。
- `BUG-R9-001`：`runner_config` 3/3 覆盖成功后拒绝重复配置、预检失败后重试，以及部分初始化失败后明确拒绝重试；实际第三方插件生命周期及最终整合未验。
- `BUG-I02-NEW-074`：独立 out-of-source 构建使用注入的目标模块路径，`p07_loader` 实际动态加载 WtDataStorage 并完成四类数据回放；补跑 `replay_completion` 中相关异常和空桶用例。Windows/Linux 和最终整合未验。
- `BUG-R60-001`：`mocker_isolation` 4/4，包括异步连接后立即释放 20 轮、释放后无旧回调及新实例重新连接；历史 N01 abort 缺日志仍未归因。卡9c 约定关闭后用新实例。
- `BUG-R60-003`：`mocker_requests` 21/21，包含多轮端口和文件描述符检查、重复释放，以及旧实例拒绝重启而新实例连接的已裁定合同；其他平台和最终整合未验。
- `BUG-I02-NEW-025`：`strategy_preflight` 包含有效策略、先有效后未知工厂失败且两个 context 均未发布、init 失败批次拒绝；不覆盖第三方策略初始化的外部副作用，已创建对象的缓存与释放另由 R134-003 跟踪。
- `BUG-I02-NEW-109`：`day_price_retry` 覆盖日线回放两日收盘时 O/H/L/C、当前价格及未知合约对照；tick 模式、真实数据、其他平台和最终整合未验。
- `BUG-I02-NEW-101`：提前停止的 `replay_public` 正控、`replay_completion` 6/6 和 `replay_status` 局部 ASan/UBSan 16/16 覆盖停止、错误和完成回调及状态区分；真实 provider、其他平台、整体生命周期及 LSan 未验。关键源对照见 `promotion-sources-3.json`。

第二批上述四项产品及测试源文件快照/工作区哈希对照见 `promotion-sources-2.json`，同样不证明整个依赖图一致。

`BUG-I02-NEW-096` 的在用缓存测试通过，但当前测试回放后只重新查询了 A，未实际重新查询 B；暂不据此证明两个代码都未被淘汰。`BUG-I02-NEW-032/082` 的损坏块和完整块普通测试通过，但未覆盖越界插桩及损坏后修复重读的缓存状态，继续保留缺口。
