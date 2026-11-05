# 决策 K18=否（BUG-R78-001）：首轮尝试与回退记录

- 尝试：在 UFT `ParserAdapter::initExt` 中不记录调用方提供的 remover（主 API 与附加 owner 都按无 remover 处理），使释放走既有的“无销毁确认即隔离”分支。
- 结果（`attempt1-summary.json`）：uftcore + m_WtUftRunner 逐例共 358 例，fail 17、SIGABRT 3。原因有二：
  1. UFT 现行“无 remover 隔离”语义把被隔离的外部 parser 视为关停未完成（`shutdown()` 返回 false，可重试），而运行器析构要求关停完成，否则终止进程。按 K18 改后，凡用外部 parser 的 UFT 运行器都无法完成关停，析构时直接 abort，与“隔离到进程退出”的本意（保活而非崩溃）不符。
  2. 约 20 个用例借助“确认销毁”的 remover 构造可回收的静默 fake，用来验证启动/关停并发、回调排空等语义；K18 之后这些路径无法再经 initExt 触达。
- 处理：回退本次产品改动，恢复为改前文件（`before/ParserAdapter.cpp`）。R78-001 保持实施中。
- 后续实施方案（与 WtCore 已裁定的 K15=A 一致，不需新裁定）：
  1. UFT 外部 parser（不论有无 remover）只通知停止一次（`release()`），之后登记为进程级常驻，视为关停完成；
  2. 运行器关停在仅剩“常驻隔离”的外部 parser 时可以完成；
  3. 把依赖 remover 销毁确认的启动/关停并发用例迁移到带 V2 生命周期表的 fake 模块，并按裁定改写其余旧断言。

## 第二轮实施（2026-11-05，按上述方案落地）
- 产品（`src/WtUftCore/ParserAdapter.{h,cpp}`）：
  - `initExt` 的 remover 不再作为销毁确认。外部 API（主 API 与附加 owner，不论有无 remover）一律按原“无 remover”规则处理：不调用 `release`、不调用 remover、不删除，隔离到进程退出，回调准入保持关闭。删除了 remover 的调用、重试、替换指针及墓碑分支。
  - 新增 `isResidentQuarantined()`（头文件定义 `WT_UFT_PARSER_K18_RESIDENT`）。`ParserAdapterMgr::release` 把处于该状态的适配器移入进程级常驻持有表（有意不析构）后从表中删除，使 `hasRetainedAdapters()` 为 false，运行器关停可以完成，不再因保留的外部 parser 在析构时 abort。
  - 第一版曾按 K15 口径对外部 API 调用一次 `release()`。K18 裁定为“同无 remover”，而 UFT 原无 remover 路径不调用 `release`，故改回不调用，以免超出裁定。
- 按裁定改写的用例（改前副本见 `before/`，每处有注释）：
  - r11：改写 5 例；其中 3 例原测 remover 抛异常、挂起、替换指针，现改为“remover 从不调用、管理器常驻、关停完成”；`releaseExceptionIsSticky…` 改为“provider 的 release 不被调用”；另 2 例断言改为 0 次 release。
  - r78：2 例安全点释放后改为 API 保留、未调用 remover。
  - r54：1 例 release 计数改为 0。
  - n03：3 例改为 API 保留、0 次 release 和析构。
  - r220：5 处运行器用例改为 0 次 release 和析构、关停完成；原 `retainedManagerAdapterBlocksCompletionAndCanRetry` 改为 `externalParserWithRemoverIsResidentAndShutdownCompletes`。
- 红控：用改前 `ParserAdapter.{h,cpp}` 重编，uftcore + m_WtUftRunner 逐例 358 例中 19 例失败，含 1 例 SIGABRT（`red/`）；恢复改后源码后 358/358 通过（`green/`）。新接口断言以 `WT_UFT_PARSER_K18_RESIDENT` 宏保护，以便旧头文件仍能编译出红控。
