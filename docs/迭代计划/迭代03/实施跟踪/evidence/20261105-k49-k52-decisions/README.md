# K49=A / K52=否 用户裁定实施证据（2026-11-05 晚）

用户裁定：
- **K49=A**：CTPMini `GetTradingDay` 失效导致登录 fail-closed 后，下一次合法登录自动就绪，不需要人工确认。现有实现就是这个合同，产品代码未改。
- **K52=否**：不要求 `CoverageTests_porter` 单进程整组运行，验收口径为逐例独立进程。I03-NEW-119 据此关闭，I03-NEW-125 保持登记。
- K50、K51、K53：用户要求先详细讨论再定，本次不改，原状态保持。

本次只运行与 T3-015 直接相关的测试二进制，不是全量回归。真实 CTPMini SDK 的 GetTradingDay 合同仍属外部条件，未验证。

## 1. 源码与冻结

- HEAD `0d54caf87`。工作区相对 HEAD 只改了 `src/TestUnits/coverage/t_TraderCTPMini/i03s5_traderctpmini_lifecycle_test.cpp`：新增 `I03S5TraderCTPMini.K49InvalidTradingDayRecoversOnNextValidLogin`。
- 冻结副本：`rsync -a --no-links` 复制 src 和 dist 到隔离目录。复制后无符号链接，与工作区 `diff -rq` 无差异。构建与运行只用这个副本。
- 哈希：
  - `src/TraderCTPMini/TraderCTPMini.cpp`：`276a5705cdc4dff05e73cc89ef6e1517699d40ac2951cdc27df1295e39cadff8`（与 HEAD 相同，未改）
  - 测试文件：`ec3ca03440abdefb4f977984793c163430b414acb5e1a083aabe0f376df04327`
  - 普通二进制：`133de5eb26e1d7b9ab9a3d596c054852f85ac7643c13e6a161d060fb5f6aeb3e`
  - ASan+UBSan 二进制：`08b6ffc8d26b13017993548cfb0f1a4584d4e1fd811951b3b2821afdf14d976d`
  - TSan 二进制：`a7e9f47ef298ba4e28994945d16af92d53f9131343cdb557ccf761c4f08457c2`

  另见 `logs/sha256.txt`。

## 2. 新用例内容

按“失效 → 断开 → 重连 → 合法登录”的顺序驱动插件。宿主 `TraderAdapter::handleEvent(WTE_Connect)` 会在收到 WTE_Connect 时重新 login；这是对源码的静态核对，宿主侧本次没有运行。断言分两段：

1. 以 `2026013X` 登录：
   - 状态为 `WS_LOGINFAILED`，登录失败回调 1 次。
   - 保留原始诊断串。
   - 不推送回报；`queryAccount` 返回 -1。
2. 断开 → 状态 `WS_NOTLOGIN` → 重连 → 以 `20260106` 登录：
   - 状态为 `WS_ALLREADY`，`isConnected` 为真。
   - 成功回调 1 次，失败回调仍为 1 次。
   - 交易日 20260106，诊断串已清空。
   - 日盘回报日期为 20260106，周二夜盘回报回推到 20260105。
   - `queryAccount` 返回 0。

## 3. 运行结果

| 运行 | 命令/目录 | 结果 |
|---|---|---|
| 普通（Apple clang 17，Debug+coverage，-Werror） | `logs/build-green.log`、`run-green/` | 构建 0 警告；K49 与 3 个原有交易日用例 4/4 通过 |
| 变异 1：失效后坚持拒绝合法登录（等同选项 B 行为） | `logs/build-mut1-sticky.log`、`run-mut1-sticky/` | 只有 K49 用例失败（状态与 `isConnected` 断言），原有 3 例仍通过，exit 1 |
| 变异 2：恢复后不清诊断串 | `logs/build-mut2-stale-diag.log`、`run-mut2-stale-diag/` | 只有 K49 用例失败（`rawDay` 断言），exit 1 |
| 恢复原始源码后全二进制 | `logs/build-final.log`、`run-binary/` | 构建 0 警告；54/54 通过（另有 4 例 DISABLED，与 F4 相同），exit 0 |
| ASan+UBSan（LLVM 22，排除 alignment） | `san.sh`、`logs/*asan*`、`run-asan/` | 54/54 通过，0 报告，exit 0 |
| TSan | `run-tsan/` | 54/54 通过，0 数据竞争报告。全部用例结束后，进程在 `exit()` 阶段 SEGV，exit 134，栈为 libsystem_c atexit → `<unknown module>` |
| TSan + `dlclose` 空实现（`run-tsan-nodlclose/`，shim 源码同 `../20261105-final-acceptance/runs/run-tsan-nodlclose/nodlclose.c`） | 同一 TSan 二进制 | 54/54 通过，0 报告，exit 0。证实上一行的退出期崩溃是已登记的 TSan atexit/dlclose 工具限制，与本用例无关 |

两个变异都只改冻结副本里的 `TraderCTPMini.cpp`。变异后已恢复，并 `cmp` 确认与工作区一致，再重新构建得到上表的“全二进制”结果。

## 4. 文档变更

由 `update_docs.py` 写入：
- `执行跟踪.json`：
  - T3-015：阻塞 → 本地验证通过，`最终验收.裁定=K49=A`，备注追加一行。
  - 汇总改为：已验收 141、本地验证通过 62、阻塞 10、待验证 0。
- `逐项验收.json`：更新 T3-015。
- `新增问题.md`：I03-NEW-119 追加 K52 裁定。
- `决策积压清单.md`：追加用户裁定一行。
- `最终验收报告.md`、`迭代计划.md`：更新计数。

`遗留问题清单.md` 第 1、3、4 节为手工同步。
