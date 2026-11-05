# i03_traders_other_20261104s5

迭代03 s5 子任务 traders-other（TraderFemas / TraderMocker / TraderYD，参考 TraderXTP）的定向构建与运行脚本。仅 macOS arm64 本机、进程内 fake API 与本机回环 UDP；不调用真实厂商 API、不连接柜台、不下单。不覆盖 Windows/MSVC、真实 SDK 回调排空或整体回归。

新增/修改的用例都在既有 coverage lane 中（自动纳入目标）：

- `t_TraderFemas/t4_003_femas_lifecycle_test.cpp`（新增，BUG-T4-003/T4-002，卡9），每例 fork 子进程；`-DWT_T4003_BASELINE` 时不调用改前不存在的接口，用于红控构建。
- `t_TraderMocker/r60_001_session_isolation_test.cpp`（新增，BUG-R60-001）。
- `t_TraderYD/r93_traderYD_api_test.cpp`（追加两例，BUG-R93-006）。
- `t_TraderMocker/t6_009_mocker_lifecycle_test.cpp`（DestructorAlone 增加查询屏障，断言值不变）；`t_TraderFemas/femas_trader_test.cpp`（夹具 TearDown 清空哨兵 API 指针）。

脚本：

- `make_cases.py <bin_dir> <variant> <out_dir>`：生成各组逐例清单（格式同 `../i03_leaks_main_20261104/run_cases.py`）。组：`t4002`、`t6009`、`r60003`、`r60001`、`t4003`、`r93006`、`lane`。
- `link_only.py <cov_build_dir> <out.json> <Module>...`：只用 ninja 记录的原始链接命令重链 `CoverageTests_t_Trader<Module>`，不触发兄弟库重编（他包进行中的改动令无关库编译失败时使用；复用构建目录中已有库）。
- `red_overlay_build.py <repo> <cov_build_dir> <before_dir> <out_dir> <Module>...`：把 `src` 无链接复制为覆盖树，用 `<before_dir>/src` 中的改前产品文件覆盖，按目标原始编译/链接命令（源码根全部改写到覆盖树，含裸 `-isystem <src>`；发现未改写即中止）构建红控二进制。Femas 加 `-DWT_T4003_BASELINE`。
- `run.sh`：完整复现命令序列（构建目录 `/tmp/wt-i03-s5-traders-other-cov`，运行根 `/tmp/wt-i03-s5-traders-other-run`）。

sanitizer：`../i03_leaks_main_20261104/manual_build.sh` 新增可选环境变量 `WT_I03_BOOST_INCLUDE`（实体 Boost 头目录）与 `WT_I03_FEMAS_EXTRA_TESTS`（追加 Femas lane TU），默认行为不变。

第二轮（主线程追加）：新增 `t_TraderYD/yd_destroy_lifecycle_test.cpp`、`t_TraderXTP/xtp_lifecycle_s5_test.cpp`，`make_cases.py` 增加组 `ydlife`、`xtps5`、`r214`；`red_overlay_build.py` 增加 TraderXTP 覆盖并对所有测试 TU 定义 `WT_S5_RED_BASELINE`（红控用 `before-round2/`）；`../i03_leaks_main_20261104/manual_build.sh` 增加 `yd`、`xtp` 模块。
