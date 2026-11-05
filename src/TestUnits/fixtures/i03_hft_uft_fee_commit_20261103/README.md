# I03 HFT/UFT replay fee commit fixture

从仓库根目录执行（本机 macOS，独立构建，最多两路）：

```sh
cmake -S src/TestUnits/fixtures/i03_hft_uft_fee_commit_20261103 -B /tmp/wt-i03-hft-uft-fee-final-20261103 -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-hft-uft-fee-final-20261103 -j2
LLVM_PROFILE_FILE=/tmp/wt-hft-uft-fee-final-%p.profraw /tmp/wt-i03-hft-uft-fee-final-20261103/check --gtest_color=no
```

CMake 构建当前 HftMocker、UftMocker、ExpHftMocker、HisDataReplayer 和 WTSBaseDataMgr；其余静态库依赖 `/tmp/ctpm-i03-build/build_x64/Debug/libs`，以及 CMakeLists 中列出的本机 Boost、gtest、nanomsg 和 iconv。该临时依赖目录不是仓库制品，迁移机器需先准备对应依赖并调整路径。23 项结果见 `evidence/test.log`；构建和配置日志、源码及库哈希、修改前快照哈希和相对快照补丁均在 `evidence/`。检查源码哈希：

```sh
shasum -a 256 -c src/TestUnits/fixtures/i03_hft_uft_fee_commit_20261103/evidence/source.sha256
```

测试向真实 replayer 的已分发 tick 快照中写入本地测试数据，然后驱动实际 HFT/UFT `procOrder`、虚 `on_trade` 和持仓资金路径；这不是完整行情分发或真实 provider。覆盖开仓、平今、平昨、反手、真实成交合约切换和显式零费、空合约身份、无对手量、缺费和手续费溢出、后续分笔溢出时整轮不提交、回调改写数据导致计划拒绝、同对象重入、不同对象嵌套、虚覆盖不提交及回调抛异常。ExpHftMocker 使用真实覆盖函数，但外部通知的 runner 是计数 stub，未测试生产 runner 注册和生命周期。

费用预检覆盖整轮分笔及累计资金有限性；它不承诺任意用户回调异常、分配失败或日志 I/O 故障下的整轮回滚。如果回调发生在某笔提交之后，该笔保持提交并扣除已成交数量，后续分笔停止。已核对构建目录没有创建符号链接；无全量回归、真实交易、live provider、跨平台或全面线程安全结论。

回调若直接改变正在撮合订单的剩余量，检查会保留观察到的订单状态并停止；这只供后续取证，不构成自动对账或恢复。每笔提交前检查剩余量足以覆盖该笔，成交后订单回调若改变剩余量则不继续余下计划。
