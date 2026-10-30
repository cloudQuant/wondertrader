# BUG-R28-007：WtRunner 必需 engine/data 初始化失败传播

## 本轮状态与证据

状态维持 `IN_PROGRESS`。本机 focused suite 43/43 PASS，另有 2 项 DISABLED。归档证据：`build.log`、`run.log` 和本文件旁的 `run.json`。独立复审无 blocker；但清单要求的 env-only 失败及 env/data 初始化失败后 direct `run()` 专项尚未运行，不能因全组 43/43 就把整个验收标成 TEST_PASS。

## 已覆盖

- `missing_env_and_data_sections_make_config_fail`：env 与 data 同时缺失，config 返回 false。此用例不单独隔离 env 缺失。
- `missing_data_section_makes_config_fail`：engine 配置存在、data section 缺失，config 返回 false。
- `invalid_data_store_module_makes_config_fail`：不可加载的 data-store module 返回 false。
- `full_cta_config_reaches_true_and_writes_portfolio_files`：以 `WtDataStorage` module 配置运行正控；从 `build-cov/build_x64/Debug/bin` 工作目录加载插件，并验证成功配置创建 portfolio 输出。
- `main_returns_nonzero_and_skips_run_when_hot_config_fails`：真实入口在 hot 配置失败后返回非零。它不是 env/data 初始化失败后直接调用 `WtRunner::run()` 的同一测试。

## 未完成的验收

`run()` 有 `_engine == NULL` 时返回的静态 guard，但没有对 env 缺失或 data/initDataMgr 失败后的 runner 直接调用 `run()`，并断言不会使用未初始化对象。现有 main 热文件失败案例只能证明该入口对该失败路径的返回行为。建议补一个隔离子进程或安全 gtest，在 env-only 缺失和 data 缺失配置分别失败后显式调用 `run()`，断言安全返回并无 engine/parser/trader/data store 调用。

其他边界：`IDataReader::init` 是 `void`，无法由该合同观察 reader 内部初始化失败；provider 异常/资源释放时序及同一 runner 重配置未覆盖。两项 DISABLED 用例仍不执行。此轮本机真实插件正控不代表其他平台或生产 provider。

## 构建记录限制

`build.log` 保留的 Ninja 输出止于 `[32/33]`，未包含最终 isolated test binary 链接行或 build 进程退出码；`run.json` 记录了实际执行的 binary SHA-256、命令和工作目录。不要把该文本日志表述为完整可复现构建记录。
