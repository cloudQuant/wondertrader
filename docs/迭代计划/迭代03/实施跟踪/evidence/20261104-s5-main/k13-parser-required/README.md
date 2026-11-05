# 决策 K13=B / K14（BUG-R104-002，I03-NEW-003）：parser 的 `required` 选项

裁定（2026-11-04）：
- K13=B：每个 parser 项可配 `required`，缺省 true（初始化失败即启动失败）；false 时降级运行并告警。
- K14：缺省仅报告；required 项提交连接命令失败时回滚（stop + drain）并启动失败。
- K16=A：同一模块在某实例失败后，其他实例允许再加载，现行即如此。

## 产品
- `src/QuoteFactory/main.cpp`：
  - `initParsers` 改为返回 bool，读取 `required`（缺省 true）。required 项初始化或登记失败时返回 false；成功的 required 项 id 记入 `g_requiredParsers`。
  - `initialize`：parser 初始化失败时先 `shutdownQuoteFactory()` 再返回 false，`main` 以 1 退出。
  - 连接命令提交失败的项中有 required 项时，回滚并返回 false；只有可选项失败时记错误并降级继续。
- `src/WtDtCore/ParserAdapter.{h,cpp}`：`ParserAdapterMgr::runWithResult` 增加可选的失败 id 输出参数，缺省参数保持兼容。
- `src/WtRunner/WtRunner.cpp`（K13 部分）：`required: false` 的 parser 初始化失败时告警并跳过，有 API 的照旧隔离保留；缺省仍 fail-closed。

## 测试（`m_QuoteFactory/r104_quotefactory_init_test.cpp`）
- 按裁定改写，改前副本在 `before/`：
  - 测试侧声明改为 `bool initParsers`。
  - `failed_active_entries_are_not_registered` 增加“缺省 required 的失败项返回 false”。
  - `kFullCfg` 中失败的 active 项标为 `required: false`，保持该用例“走到 initialize 末尾并返回 true”的意图。
- 新增：
  - `k13_optional_failed_parser_does_not_fail_startup`：返回 true，不登记。
  - `k13_required_failed_parser_fails_startup`：子进程中 `initialize=false`。
- 红控：用改前 `QuoteFactory/main.cpp` 重编，required 用例失败（子进程报告 `initialize=true`，`k13-red.log/xml`）；恢复后通过（`k13-green2.log`）。`initParsers` 用例因改前返回 void，无法做有效红控，未做。
- 逐例回归：m_QuoteFactory、m_WtRunner、dtcore 共 351 例 pass 348、skipped 3（`regression-summary.json`）。

## 未完成
- K14 在 WtRunner、WtRtRunner、WtUftRunner、WtDtRunner/WtDtServo 尚未实现（required 项连接提交失败时回滚），K13 在后四个宿主也未实现。
- WtRunner 的 optional 路径未加专门用例。
