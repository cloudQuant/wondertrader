# 最终验收前 sanitizer 预扫与主代理修复（2026-11-05）

本目录记录最终冻结**之前**的 sanitizer 预扫，以及主代理据此做的修复。预扫用于提前发现问题，**不是**最终整体回归验收；最终冻结、干净构建与整体回归另见 `../20261105-final-acceptance/`。

## 预扫快照

- 冻结：2026-11-05T12:16:39+0800，HEAD `ccaad010`，src+dist 6649 个文件，清单摘要 `0b8ab2dba2d8919dd0c67a6502efe1dc7c8692289004489a4fbf65184edf2d0e`（`prescan/meta/`）。与第一轮冻结快照相比只多复修 1 的 5 个文件。
- 构建：Homebrew LLVM 22.1.8，Debug + coverage，`WT_WARNINGS_AS_ERRORS=OFF`（与 Apple clang 告警集不同，产品 -Werror 由最终 Apple clang 构建验证）。
  - ASan+UBSan：`-fsanitize=address,undefined -fno-sanitize-recover=undefined`（预扫**包含** alignment 检查）。
  - TSan：`-fsanitize=thread`。
  - 配置脚本 `prescan/san-configure.sh`、`prescan/llvm-implicit-usrlocal.cmake`。
- 运行：`prescan/run_full.sh`（与第一轮相同的用例集合和模块预置，逐例独立进程/独立 cwd，600 s 超时）。机器同时在跑多路构建，负载很高，预扫中的时序类失败不作结论。

## 环境问题：LLVM 构建的 Boost 头与库不一致（已修正，不另编号）

首次 ASan 构建 `CoverageTests_t_CTPLoaderYamlPositive` 链接失败（`boost::filesystem::path::find_root_directory()` 未定义）。原因：AppleClang 把 `/usr/local/include` 视为隐式目录，CMake 不输出 `-I/usr/local/include`；Homebrew LLVM 不视为隐式目录，CMake 便输出 `-I/usr/local/include` 并排在 coverage Boost bridge 之前，导致该目标用 `/usr/local` 的 Boost 1.81 头编译、链接 Homebrew 1.88 库。修正：sanitizer 构建用 `CMAKE_PROJECT_INCLUDE` 把该目录标为隐式（CMake 不再输出 `-I`），再以 `-isystem /usr/local/include` 尾置，搜索顺序与 AppleClang 一致。首次失败构建日志未纳入（已用修正后的构建替换）。上一会话 btcore/btporter san22 构建核对：其 TU 没有 `-I/usr/local/include`，不受影响。

## ASan+UBSan 预扫结果（`prescan/run-asan/`）

6116 例：pass 6015、skipped 4、ASan 4、UBSan 92、fail 1。归类见 `prescan/run-asan/triage.txt`：

| 类别 | 例数 | 处置 |
|---|---:|---|
| UBSan alignment（`#pragma pack(1)` 存储块/共享块结构的未对齐类型化访问，产品 `WTSStruct.h:197` 等，及测试对 packed 成员取引用） | 约 90 | **I03-NEW-109**，既有布局问题，不在 213 范围；最终验收沿用既有口径排除 alignment，测试侧两处取引用已修 |
| `URLEncode` 栈越界读 | 3 | **I03-NEW-108** 已修 |
| `P09DataReaderTest.get_adj_factor_by_date_branches` heap-use-after-free | 1 | **I03-NEW-110** 测试缺陷已修 |
| `WtDataWriterAD.cpp:91` 对 `0xbebebebe…` 指针成员调用 | 1 | **I03-NEW-111** 产品缺陷已修 |
| `r28_wtrunner_strategies.real_cta_factory…` 创建 `cta/` 失败 | 1 | **I03-NEW-112** 产品缺陷已修 |
| `TestUnits/test_fmt.cpp:128` 越界 | 1 | **I03-NEW-114** 旧测试已修 |
| 跳过 | 4 | 与复修全量相同（子进程用例 2、R44 合同、卡15 链接负控） |

## TSan 预扫结果（`prescan/run-tsan/`）

6116 例：pass 5893、skipped 4、TSan 报告 106、signal-6 105、fail 6、timeout 2。

| 类别 | 例数 | 处置 |
|---|---:|---|
| 用例 PASSED 后进程退出时 SEGV/BUS（加载真实插件工厂的 btcore mocker、t_CTPLoader 等） | 约 105 | 工具限制，已证实：TSan 拦截的 `__cxa_atexit` 回调在模块 dlclose 后于退出时被调用。插入 `dlclose` 空实现（`logs/tsan-atexit-dlclose/nodlclose.c`）复跑两例：不插 exit=134，插入后 exit=0；普通与 ASan 构建均不崩溃 |
| `WtEngine` 持仓表/资金/时间字段竞争（R41/R66/R101/R170/R181/R192/S5LiveFeeSchedule，含 I03-NEW-100 的用例） | 约 20 | **I03-NEW-115** 产品缺陷，修复见 slice2 证据 |
| `MQServer`/`MQClient` 标志与队列竞争 | 9 | **I03-NEW-116** 产品缺陷已修（本目录） |
| `TraderAdapter::_orders` 锁外判空/懒创建 | 3 | **I03-NEW-117** 产品缺陷，slice1 修复 |
| r23 写端夹具 sink 无锁；`abandon_writer` 使 worker 活过 sink | 18 | **I03-NEW-113** 测试夹具已修 |
| R66 HFT ticker 运行中再次 init；ParserUDP 测试 sink 运行中替换回调；porter 全局信号标志；T26 stub | 5 | 测试侧，slice2 修复 |
| boost threadpool 工作线程未 join（执行器、CTA 引擎、IndexWorkerMgr） | 约 50 | 第三方库关闭策略，低严重度，仅退出期；登记不改 |
| fail 6 / timeout 2 | 8 | 多为 I03-NEW-112 同一 255 字节路径问题及高负载超时；最终回归复核 |

## 禁用用例诊断（`prescan/run-disabled-asan/`）

`--gtest_also_run_disabled_tests` 逐例运行 224 个 DISABLED_ 用例（ASan 树，90 s 超时）：pass 106、fail 35、sanitizer 43、skipped 32、timeout 4、其他 4。逐例处置见 slice3 证据 `disabled/`。

## 主代理修复（红/绿）

| 编号 | 文件 | 修改 | 红 | 绿 |
|---|---|---|---|---|
| I03-NEW-108 | `Share/charconv.hpp` | `URLEncode` 对单字节直接判断最高位，不再把 `&cc` 当 C 串 | 预扫 3 例 ASan stack-buffer-overflow | 最终 ASan 回归 |
| I03-NEW-109（测试侧） | `l02_csv_shareblock_test.cpp` | 先拷贝 packed 成员再交给 gtest | 预扫 UBSan reference binding | 最终回归（alignment 已排除，普通构建通过） |
| I03-NEW-110 | `p09_reader_init_adjfactor_test.cpp` | 插入新键前拷贝列表 | 预扫 heap-use-after-free | 最终 ASan 回归 |
| I03-NEW-111 | `WtDataStorageAD/WtDataWriterAD.cpp`（普通写端由 slice2 同步） | 构造函数初始化 `_bd_mgr` | 预扫 UBSan `0xbebebebe` 成员调用 | 最终 ASan 回归 |
| I03-NEW-112 | 五份 `WtHelper.cpp`（WtCore/WtUftCore/WtBtCore/WtDtCore/WtDtServo） | 4096 缓冲、检查 `getcwd` 返回值，失败返回 `"./"` 且不缓存 | 新增五族各 2 例在改前源码独立编译运行 0/2（`logs/new112/*-before.*`） | 改后 2/2（`*-after.*`）；用例进入各族 coverage 目标 |
| I03-NEW-113 | `r23_storage4/r23_writer_fixture.h` | 日志互斥、计数原子；TearDown 同样 release 被放弃的 writer | 预扫 TSan 18 例 | 最终 TSan 回归 |
| I03-NEW-114 | `TestUnits/test_fmt.cpp` | `to_chars` 上界改为 `sizeof(buffer)` | 预扫 UBSan index out of bounds | 最终 ASan 回归 |
| I03-NEW-116 | `WtMsgQue/MQServer.{h,cpp}`、`MQClient.h` | 终止/超时/心跳字段原子化，cast 线程在锁内判空，建线程在锁内 | 预扫 TSan 9 例 | 最终 TSan 回归 |

改前改后副本见 `before/`、`after/`，哈希 `sources.sha256`。产品改动均已在 Apple clang `-Wall -Wextra -Werror` 下语法检查无告警（WtHelper 五份；WtMsgQue 使用项目 `fmt_enum_compat.h`），完整 -Werror 构建见最终验收。
