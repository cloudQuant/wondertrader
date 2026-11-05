# TraderFemas 下单同步返回诊断

2026-11-02，本机 arm64，进程内 fake API，无真实交易。当前 `TraderFemas::orderInsert` 把 SDK 的非零值记录为“Order inserting failed”，但向 `ITraderApi` 调用方返回 0；`WtCore` 和 `WtUftCore` 两处 `TraderAdapter::doEntrust` 都据 `ret < 0` 判失败并丢弃 localid。Femas 3.02 随仓库 SDK 头只声明 `ReqOrderInsert` 签名，未说明各返回码、非零是否意味着无发送或是否可有迟到/重入回报。`ITraderApi` 头也未写返回值合同；其他不同 SDK 的实现不能替代 Femas 版本合同。对版本匹配的官方材料尚未找到，按不足处理。

受控 fake 把 0、-3、任意正数 7 作为该调用的返回值；正数 7 是测试假设，不声称厂商会返回。按当前代码的“非零发送失败”日志对应宿主负值报告的候选期望，两条诊断断言均失败：

- success：SDK 0 → wrapper 0、调用一次，无回报，exit 0。
- negative：SDK -3 → wrapper 0，日志有错误，exit 1（候选负值报告断言失败）。
- positive：SDK 7 → wrapper 0，日志有错误，exit 1（同上）。
- reentrant：fake 在请求栈内主动注入一个无错误 `OnRspOrderInsert`，随后返回 -3；sink 收到一次无错误回报而 wrapper 返回 0，exit 0（仅断言现状与回调被转发）。此 fake 刻意构造冲突，**不证明真实 Femas 支持此时序**。

把非零直接映射成负值可能导致现有宿主丢弃已收到回报的 localid，进而影响订单身份和账本；在未证明精确 SDK 合同、宿主缺乏“提交状态未知”处理和隔离机制前，没有修改生产代码。下一步需要取得精确版本的官方返回码/回报及线程时序合同，或者设计具备三态（已确认未提交 / 已提交 / 不确定）及订单身份保留、暂停和对账能力的适配器方案；任何回滚须依赖可证明的未提交条件，不能仅凭 SDK 非零。现有 fake 仅反映包装器本地行为。

## 重建

当前工作树 HEAD `ccaad010`，使用实体 `/opt/homebrew/Cellar/boost/1.88.0/include`，未创建符号链接。使用 Apple `/usr/bin/c++`，产品 TU 与专用 fixture 独立编译链接；`-undefined dynamic_lookup` 是最小链接，未运行未调用的产品路径。系统/预编库未插桩，不是整仓构建或 SDK 验收。`hashes.txt` 记录实际目标源码、fixture、stub 和测试二进制。

```sh
mkdir -p /tmp/wt-i03-femas-insert-20261102
COMMON='-std=gnu++17 -g -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/Cellar/boost/1.88.0/include -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -isystem /opt/homebrew/include -isystem src -include src/Share/fmt_enum_compat.h'
/usr/bin/c++ $COMMON -Dinst_hlp=femas_insert_product -c src/TraderFemas/TraderFemas.cpp -o /tmp/wt-i03-femas-insert-20261102/red.o
/usr/bin/c++ $COMMON -Dinst_hlp=femas_insert_test -c src/TestUnits/fixtures/femas_insert_return/insert_return_test.cpp -o /tmp/wt-i03-femas-insert-20261102/test.o
/usr/bin/c++ -g /tmp/wt-i03-femas-insert-20261102/red.o /tmp/wt-i03-femas-insert-20261102/test.o -o /tmp/wt-i03-femas-insert-20261102/red -Wl,-undefined,dynamic_lookup -L/opt/homebrew/Cellar/boost/1.88.0/lib -Wl,-rpath,/opt/homebrew/Cellar/boost/1.88.0/lib -lboost_filesystem -lboost_thread -liconv
```

上述三个命令退出 0；每个模式通过 Anaconda base Python `subprocess.run` 单独运行，8 秒超时，命令及结果分别记录于 `red-*.log`。fixture 的两个失败断言表达的是待确认的候选本地报告合同，不能被当作已确认的厂商合同或全链路验收。未运行真实 SDK、真实交易、内存工具及整体回归。
