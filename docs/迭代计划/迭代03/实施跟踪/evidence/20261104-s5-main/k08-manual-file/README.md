# 决策 K08 a/b/c（BUG-Q04-007）：UFT 手工持仓文件 `mannual.yaml`

裁定（2026-11-04）：a=B 解析失败不归档、本次切日失败；b=B 含未知合约整份拒绝；c=C 先改名归档、再按归档内容应用（恰好一次）；e=A（加载失败不通知就绪、下次就绪重试，现行）。

## 产品（`src/WtUftCore/UftStraContext.cpp::load_local_data`，CRLF 保持）
- 准备阶段读取原始字节并解析（沿用 `WTSCfgLoader::load_from_file` 的编码处理）。以下任一情况整份拒绝：空文件、无法解析、根不是对象、缺 `details` 列表、条目缺交易所/代码、合约不存在、`direct` 不是 0/1、`volume`/`openprice` 非有限或为负。拒绝时记 ERROR，不导入、不建本地块、不归档，`load_local_data` 返回 false（切日失败，策略不收到就绪）。空列表 `details: []` 合法，表示清空持仓。
- 准备阶段所有可失败步骤完成后、提交前：把文件改名为 `mannual.yaml.<时间戳>`（重名时加序号，不覆盖旧归档），再读回归档字节，必须与已校验字节一致。改名失败或内容不一致（并尽力改回原名）都会让本次切日失败，且不应用任何内容。此后提交与发布不再分配内存或出现可失败步骤。
- 删除原“发布成功后再改名、改名失败仅记错误”的逻辑（该逻辑会在下次切日重复导入，覆盖期间的成交）。

## 测试
- 改写（旧断言来自历史行为 A，按裁定更新，改前副本 `before/`）：
  - `q04_uftctx_local_report_test.cpp`：`manual_positions_are_imported_then_file_is_renamed` 去掉未知合约条目。`manual_unknown_contract_does_not_hide_later_valid_details`（部分导入 5 手）改为 `manual_file_with_unknown_contract_is_rejected_as_a_whole`：整份拒绝、文件保留、不建块；修正后重试导入 5 手并归档。
  - `r78_uftcore_ticker_ctx_test.cpp`：空文件、无 details 两例改为拒绝并保留。
  - `r54_uftctx_callback_gaps_test.cpp`：导入用例去掉未知合约条目；无 details 用例改为拒绝、不归档、`readyCnt` 为 0（e=A）。
- 新增：`manual_file_with_invalid_entries_is_rejected_as_a_whole`（7 种非法内容，空列表正控）；`manual_file_is_not_applied_when_it_cannot_be_archived`（目录只读使归档改名失败：持仓保持 4 手、文件保留；恢复权限后重试导入 3 手并归档）。
