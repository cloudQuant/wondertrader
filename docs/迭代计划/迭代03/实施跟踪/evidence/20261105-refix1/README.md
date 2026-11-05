# 第一轮问题集中复修（I03-NEW-103~107），2026-11-05

在工作区修改。第一轮的冻结快照未改动，保留为第一轮证据。改前在 `before/`，改后在 `after/`，哈希见 `sources.sha256`。

| 问题 | 修改 | 验证 |
|---|---|---|
| I03-NEW-103 R42 用例参数不符合 schema | `r28_runner_main_test.cpp`：按 WSN06-002 的 schema 提供必填 `code`，产品校验不放宽 | 红：第一轮补充运行失败（`../20261105-round1/supplement-env/`）；绿：2/2 通过 |
| I03-NEW-104 R143 被误报跳过 | `r143_porter_bars_test.cpp`：基础文件补 holiday，品种补 `holiday:CHINA`（卡1）；actions.json 用最小合法策略，空策略按 fail-closed 被拒（lldb 定位 `initActionPolicy`，`logs/r143-initActionPolicy.lldb.log`）；只有模块确实不存在才跳过，模块存在而配置失败判为失败 | 红：第一轮跳过；中间状态：改为失败语义后依次暴露缺策略、缺品种日历（`logs/r143-*.log`）；绿：3/3 通过 |
| I03-NEW-105 写死路径 | `r15_mgr_executers.cpp`：依次取 `WT_COV_PLUGIN_BIN`、本测试二进制所在构建的 `bin/`（dladdr），不再写死仓库 build-cov；`p04_replayer_csv_replay_test.cpp`：候选目录增加本构建的 `bin/` | 红：第一轮跳过；绿：R15 3/3、P04 1/1 通过 |
| I03-NEW-106 回归工具缺运行时变量 | `regression-tools/run_regression.py`：构建中存在 r160 假 MQ 目录或 HFT 工厂时自动设置 `WT_R66_FAKE_MQ_DIR`/`R42_HFT_PLUGIN`（已设置则不覆盖），并写入 manifest 的 `auto_env` | 在未设这两个变量的环境中运行，`auto_env` 生效，6 例全部执行（`logs/refixed-13-cases-*.json`） |
| I03-NEW-107 WtStraDtSel 告警 | 删除未使用的 `curPx`（`closes->at` 只读）与 `commInfo`（`stra_get_comminfo` 是纯查询，`SelStraBaseCtx` 实现直接返回 `get_commodity_info`），无副作用；保留 CRLF | 全量重建告警 0（`logs/build-warning-count.txt`） |

**复修后工作区全量逐例**（不是冻结快照，不作验收）：6116 例，通过 6112、失败 0、跳过 4、禁用 224（`logs/full-run-after-refix-summary.json`）。剩余 4 例跳过如下，均与上述修复无关：
- 子进程用例 2 例：只由父用例拉起；
- R44：合同未定义；
- 无符号链接约束：卡15 用户指示忽略。
