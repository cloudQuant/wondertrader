# Journal 标记失败与首次建档预检（局部证据）

2026-11-02，macOS arm64。本次仅修改 `UnresolvedReportJournal.{h,cpp}` 和专属测试，未接入 TraderAdapter、账户准入或任何真实交易接口。`baseline/` 是本轮修改前工作区原文件；磁盘格式不变。

## 反例及修改

- 创建 in-flight 文件成功后，旧代码在权限设置或文件校验失败时尚未建立取证快照；追加返回 `StorageFailed`、实例不健康，但 `recoveryInspection` 返回 `NotOpen`，无法立即导出先前报告。`red.log` 为受控故障：在旧代码的权限设置处通过新增专属测试钩子模拟失败，测试断言失败，exit 1；不是声称在此机器上真实触发了 `fchmod` 错误。
- 现于独占创建 marker 成功后立即登记该文件存在及需人工恢复，尚未写入的 marker 字节保持空；之后权限、身份或写入失败也能扫描并导出有效前缀。追加、ACK 相关测试均检查进程内及关闭重开后的报告证据和不可正常查询状态。
- 原 `createNew` 先创建 lock、WAL，再发现已有 guard 或 marker，可能在已存在的孤立组件旁制造新的空文件；组件路径探测出错也可能已经创建若干文件。`preflight-red.log` 的两项旧行为测试均失败，exit 1。现于新建任何组件之前检查 lock、WAL、guard、in-flight 和 cleanup 五个路径；已有文件按既有大类返回 `AlreadyExists` 或 `RecoveryRequired`，探测错误返回 `StorageFailed`，不创建新组件，不覆盖已有字节。预检发现孤立 marker 时同时保留只读取证视图。
- 预检与后续文件创建之间仍可能发生跨进程变化，`O_EXCL` 仍负责每个文件的独占创建；此改动**不证明多文件事务原子性、敌对进程隔离、实际 ACL 或掉电 durability**。失败发生在真正创建之后时仍可能留下部分建档文件，必须保持人工恢复流程。

## 本机验证

```sh
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
  -I/opt/homebrew/Cellar/googletest/1.17.0/include \
  src/WtCore/UnresolvedReportJournal.cpp \
  src/TestUnits/coverage/n02_wtcore_engines/unresolved_report_journal_test.cpp \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -pthread \
  -o /tmp/wt-i03-journal-preflight-green
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/wt-i03-journal-preflight-green '<filter from final.log>'
```

`final.log` / `final.xml` 是 15 个针对 marker 权限失败、首次建档已有文件/探测错误、正常建档、torn prefix、ACK 和写入/清理失败及进程退出重开的关联测试，15/15 PASS、exit 0。只编译本类和专属测试；系统库与预编 GTest 未插桩。未运行故意创建符号链接的测试；未创建符号链接。完整测试、Windows/Linux、真实 OS 故障与硬断电均未验证。

仓库已有的 bootstrap、权限、torn-prefix、ACK flush 失败重开测试与部分实现并不等于原 BUG-L01-014 已关闭：现有 primitive 未接入生产回报，账户级暂停、身份认证、外部对账和恢复门禁未完成，跨进程操作期间的路径竞态及生产部署同样未证明。
