# BUG-N03-006 独立翻译单元链接定向复验

以当前工作区 `src/WtUftCore/TraderAdapter.cpp` 和独立测试 TU `src/TestUnits/coverage/n03_uftcore_index/n03_uftcore_helper_adapter_test.cpp` 在独立 /tmp 编译并重链。两个 TU 均使用 `-Wall -Wextra -Werror -std=gnu++17`；实体 Boost 1.88 bridge `/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include` 优先。精确命令数组见 `commands.json`，`build-0.log` 至 `build-2.log` 对应两次编译和链接，退出均为 0；源码和测试构建前后 SHA 及可执行文件哈希在 `source-hashes.json`。目标产品/测试源码构建前后、运行后哈希一致。

独立 `nm -C` 结果：`test-symbol.txt` 对 `wtp::TraderAdapter::isTradeEnabled(char const*) const` 为 U（未定义引用），`adapter-symbol.txt` 为 T（当前产品对象定义），`binary-symbol.txt` 为 T（最终可执行文件定义）；避免将内联实现或旧对象误当作此链接成功。

在独立 cwd/TMPDIR 执行，20 秒上限：

```sh
TMPDIR=/tmp/wt-i03-n03-006/run/ LLVM_PROFILE_FILE=/tmp/wt-i03-n03-006/run/profile-%p.profraw perl -e 'alarm 20; exec @ARGV' -- /tmp/wt-i03-n03-006/bin/CoverageTests_uftcore --gtest_filter=n03_trader_adapter.fresh_adapter_public_accessors --gtest_output=xml:/tmp/wt-i03-n03-006/run/result.xml
```

exit 0、XML 1/1 PASS、0 disabled/skipped，日志在 `run/`；未创建符号链接或改动仓库。该用例只覆盖新建 adapter 风控关闭的公开调用。其他 UFT 产品对象、外部库来自 v8 构建，非完整当前树一致构建；未运行完整 UFT 套件、其他平台或真实交易接口，未做 sanitizer 或整体回归。
