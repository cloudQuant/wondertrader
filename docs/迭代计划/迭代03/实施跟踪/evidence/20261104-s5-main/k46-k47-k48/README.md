# 决策 K46–K48 实施（2026-11-05 用户裁定：全部按建议）

## K46=B（I03-NEW-081）：`isAsync()` 表示是否启动过后台 worker
- 产品不改：`WtBtRunner::run` 在 prepare 期间被 stop 时恢复 `_async` 旧值、不启动 worker，现状即 B 语义。
- 测试：`q06_btporter/r222_bt_runner_shutdown_test.cpp` 的 `ImmediateStopWaitsForPrepareThenJoinsTheWorker` 末尾 `EXPECT_TRUE(runner.isAsync())` 改为 `EXPECT_FALSE`（改前副本 `before/`）。
- 对照：int5（改前断言）该例失败（`../int5/results.jsonl`）；改后见 int6。仅测试断言按裁定调整，无产品红绿。

## K47=B（BUG-I02-NEW-104）：日线 d→d、times=1 明确拒绝并诊断
- 产品：`src/WtDtHelper/WtDtHelper.cpp` `resample_bars` 在读取切片、建立会话后、`extractKlineData` 前判断 `times <= 1`：记“重采样倍数{}无效：必须大于1，同周期重采样不受支持，请直接读取原始K线”，释放切片与会话后返回 0（行尾保持 CRLF）。
- 测试：`q05_resample_bars_test.cpp` 的 `DayResampleOfTenDailyBarsPassesThrough` 改为期望 0、无回调、明确诊断，并加 times=2 正控（10→5 条）；`q05_resample_leak_paths_test.cpp` 的 `SamePeriodTimesOneFailsWithoutCallback` 诊断断言由笼统“重采样失败”改为新诊断。
- 红控：改前 `WtDtHelper.cpp` 重编，`Q05Resample*` 2 例失败（`q05-red.log/xml`）；恢复改后（哈希 0bf0db90…）13/13 通过（`q05-green*.log`）。
- 泄漏：主线程 `leaks --atExit` 复跑 `Q05ResampleLeakPathsTest.*` 与改写用例，均 0 leaks（`leaks_*.log`）。

## K48=A（I03-NEW-098）：覆盖率 lane 固定临时目录改按进程区分
- 普查：`../k48-fixed-tempdir-census.txt`（单行形式 17 处）；补充多行形式普查另发现 `n04_hisdatamgr_test.cpp`（`n04_<name>`）与 `t6_ctploader_reachable_test.cpp`（固定文件名）。
- 修改 12 个测试文件：目录/文件名追加 `getpid()`（`t6` 文件名前缀进程号以保留扩展名）。`l06_no_such_dir` 为刻意不存在的路径，不改。仅改测试隔离，不改断言。改前副本 `before/*.before-k48`，改后 `after/`，哈希 `sources.sha256`。

## 同期发现（I03-NEW-099，测试时序）
- `R192*Ticker`：卡3 3a 把夹具 tick 由 HH:MM:60 改为 HH:MM:59 后，ticker 在 tick 后约 1000ms 才闭合分钟，原 400×2ms 等待与之竞态（06:51 串行 5/7 失败）；改为 5 秒期限等待，断言不变，连续 3 次 7/7（`r192fix*.log`）。
- `R90ExecPool.*`：见 `../r90-arbi-race/README.md`。
