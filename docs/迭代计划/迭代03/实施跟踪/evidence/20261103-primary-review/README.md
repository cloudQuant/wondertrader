# 2026-11-03 主线程独立复核

完整迭代目标与213项基线不变，新增8项独立跟踪；整体回归仍为 NOT_RUN。`archive-hashes.json` 是归档时摘要，不是原子构建输入证明。

| 范围 | 主线程实际验证 | 限制 |
|---|---|---|
| WSN06-004 | 独立Debug构建，`-Wall -Wextra -Werror`；配置矩阵退出0；8个DualThrust与5个TwoMa关联用例13/13 | 其他平台、超大历史资源预算及完整回归未验 |
| L05-002 CSV结果 | 独立编译新旧导出，局部ASan/UBSan 14/14；正常CSV字节与修改前快照一致 | 旧void接口保留，新v2返回状态；预编依赖未全插桩、LSan关闭，真实存储故障、Windows、外部调用方迁移及事务发布未验 |
| L03-009/010/012 Session | 独立编译当前BaseDataMgr及头文件，最终48/48 | 跨零点autoAdjust空档策略、旧插件混用、日期API失败传播及完整树未验 |
| L03-002 商品 | 独立编译当前BaseDataMgr，最终8/8，实际de_DE.UTF-8小数逗号locale无跳过 | 字段检查不是显式ready；旧内联canShort与插件准入未闭合 |
| R3-002/003 | 重跑专用脚本退出0 | 当前生产函数提取与替身验证；不是完整适配器/供应商验收，明确未提交回退、日总量并发、R3-008账本仍待完成 |
| 小数目标 | 核对13份相关源码/fixture哈希均一致，独立运行已构建目标13/13 | 复跑非独立重编；Diff并发批次原子性、负/小数组合、多组净额、双限额仍未完成 |
| HFT/SEL重复ID | 主线程独立重编真实管理器及本地插件，两个检查程序均退出0 | 保留原策略、创建前占位；不能证明管理器析构并发、完整runner启动和整张卡9 |

追加复跑：`src/TestUnits/fixtures/i03_strategy_id_20261103/run.sh /tmp/i03-strategy-id-root-20261103`；小数目标使用 `/tmp/i03_fractional_targets_current_20261103/build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore`，按该目录`final.log`的13项filter，在独立工作目录`/tmp/i03-fractional-root-run-20261103`运行。哈希匹配不证明未列出的全体依赖均匹配。

复跑方法分别见 `src/TestUnits/fixtures/i03_dualthrust_config_20261103/README.md`、`i03_csv_status_20261103/README.md`、`i03_session_contract_20261103/README.md`、`l03_commodity_state/run-config.sh` 和 `i03_frequency_20261103/README.md`。主线程独立输出目录依次为 `/tmp/wt-i03-dualthrust-root-20261103`、`/tmp/i03-csv-status-root-20261103`、`/tmp/i03-session-root-20261103` 和 `/tmp/i03-frequency-root-20261103`。商品脚本指定 `/tmp/wt-i03-r58-20261102-build`；Session同样使用该预编库目录。

本目录XML和日志对应实际运行。DualThrust关联过滤为 `n06_cta_ctx.dualthrust*` 加三个 `n06_cta_stra.init_rejects_*two_ma*`、`n06_cta_ctx.twoma_on_schedule_golden_cross_enters_long`、`n06_cta_ctx.twoma_short_window_needs_more_history_and_releases_slice`。CSV使用LLVM22、ASAN_OPTIONS=`detect_leaks=0:abort_on_error=1`、UBSAN_OPTIONS=`halt_on_error=1`；golden由保存的修改前源码生成。Session与商品复用旧WTSTools/WTSUtils库，不代表当前完整依赖树已通过。

复核退回并修正过：缺省days仍为零、商品数字解析依赖进程locale、商品fixture依赖未跟踪文件、全天闭盘端点被归一为零。Session全天`2400/240000`本轮明确为闭盘端点；修改前快照归一为零，本轮是行为修正。费用/日期能力仍需跨入口接入；卡9完整owner保活、回调排空和账户门禁不由重复ID修复替代。

继续实施入口：Diff `set_position`在预检后释放`_position_update_mutex`再逐项提交，且`getUnit`先于复查；须设计整批目标提交与回调派发顺序。Local/Arbi的`_code_to_groups`仍为单值覆盖，单组整数分支不能完成多组净额/双限额。日期能力应保持旧`IBaseDataMgr`虚表，不能强转未知实现；费用必须区分CTA/SEL理论持仓与HFT已确认成交。正常注册的ParserAdapter停止路径尚无provider排空能力证明，WtRunner/WtRtRunner宿主保活和账户准入必须一起处理。以上均未因本轮局部通过而关闭。
