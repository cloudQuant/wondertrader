# 06_mockers 本轮执行记录

- CoverageTests 刷新由主代理执行；全量刷新证据：`docs/迭代计划/迭代02/复核185/evidence/initial-build.json` 与 `.log`。
- 本组定向二进制：`build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_btcore`。
- SHA-256：`430676e26c035f16dc733f23f4e72196a86ed5d4373f3b86ebce7d554be370d4`。
- 10 个 filter 分别用 `/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python` 包装 `subprocess.run(timeout=15)`；每次独立临时 cwd/TMPDIR、`LLVM_PROFILE_FILE` 和 GTest XML。`runs.json` 记录命令、cwd、SHA、return code、秒数、测试数和日志路径；各 `.log` 保存 stdout/stderr。
- 8 PASS：`notifier_inactive`、`exec_missing_module`、`cta_normal_trade_lifecycle`、`cta_unknown_code_guard`、`incremental_missing_base`、`incremental_optional_details`、`uft_order_input_guard`、`uft_exit_no_position`。
- 2 FAIL：`incremental_roundtrip` 是 test oracle 顺序/键名写错；`uft_normal_dynprofit` 暴露当前 UFT 全平 detail 清理 bug（范围外，已报主代理）。
- 未运行 active MQ、disabled UB、坏快照、sanitizer；target fresh build 的结果不等于这些缺陷已复现。
