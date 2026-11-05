# BUG-L00-004 公开头首包含复核

## 本轮范围与结果

从本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 树（Git 树 ID `67f193194f16197bc5a5b0ae9c41ddcf3faed546`）通过 `git archive` 提取到独立目录，对历史记录所列全部 13 个使用 `boost::noncopyable` 的头分别生成单行 `#include` TU，用 C++17、`-Wall -Wextra -Werror -fsyntax-only` 编译。13/13 单独退出 0，脚本总退出 0；[逐头结果](results.txt)、[脚本](check_headers.sh)、[头文件 SHA-256](header-sha256.txt)在此目录。同名 `.log` 为空表示编译器无诊断。

环境为 Apple clang 17.0.0、Darwin 25.5.0 arm64、macOS SDK 26.2；使用 `/opt/homebrew/Cellar/boost/1.88.0/include`、`/opt/homebrew/Cellar/fmt/11.2.0/include`、`/opt/homebrew/Cellar/spdlog/1.15.3/include`，其中 Boost 的 include/boost 目录不是链接。没有创建符号链接。该证据只覆盖所记录的源码快照和本机编译，不证明 Windows/Linux，也不是整树构建。

```sh
mkdir -p /tmp/wt-i03-l00-004-20261102/snapshot
git archive ccaad010 src | tar -x -C /tmp/wt-i03-l00-004-20261102/snapshot
sh 'docs/迭代计划/迭代03/实施跟踪/evidence/L00-004-20261102/check_headers.sh'
```

## 历史四个 R3 失败的归类

历史 `迭代02/实施跟踪/evidence/20261030-l00-004/smoke-and-trader.log` 中失败的是**行为测试而非四个头编译**：订单频率边界和撤单频率边界分别返回 12/33（都仍允许超限下一次）；拆单测试的实际未完成量为 5 而非 3；按标准码查询的订单表 size 为 0 而非 1。历史缺陷台账分别关联 `BUG-R3-002`、`BUG-R3-008`、`BUG-R3-009`，其中最后一项历史分析记载订单已入表但原始码与标准码匹配失败。上述归因以旧日志/旧台账为依据，不构成当前树行为运行结论，也不证明这些交易问题已经关闭。

本轮没有运行 R3 行为测试：频率窗口和未完成量需要依各自条目的合同与修复计划单独处理；首包含测试只验证编译自足，不覆盖运行行为。最初两条失败头的原始 stdout 无法从本轮成功结果补回；完整工程、后续变更及其他平台仍须分别验证。
