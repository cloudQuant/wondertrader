# BUG-R160-002：UFT 默认 confirm 定向复验

2026-11-02，本机 macOS arm64。以 `git archive ccaad010 src` 导出实体源码快照（commit `ccaad010e9169f610aded4d8dc0f5647f70a5cff`），相关 notifier、helper、测试和 fake 源的 SHA-256 与当前工作区相同，见 [sha256.txt](sha256.txt)。仅编译所需产品、测试翻译单元和专用 fake 动态模块，使用实体 Cellar Boost 头目录；独立源码和构建目录 `find -type l` 为空，本次未创建符号链接。

构建命令见 [build.sh](build.sh)，运行 `bash build.sh` 退出 0，输出见 [build.log](build.log)。链接复制自 `/tmp/wt-i03-r41-drain/libs/` 的既有 WTSTools/WTSUtils 静态库，未证明它们与本快照完整依赖闭包一致；GoogleTest 和 Boost 静态库也为本机预编库。该结果不是整树冷构建。

在 `/tmp/wt-i03-r160-uft-20261102/run` 独立目录运行：

```sh
LLVM_PROFILE_FILE=/tmp/wt-i03-r160-uft-20261102/run/profile.profraw perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-r160-uft-20261102/focused --gtest_filter=R160UftNotifier.FakeMqReceivesDefaultConfirmAndIsDestroyed --gtest_output=xml:/tmp/wt-i03-r160-uft-20261102/run/result.xml
```

进程退出 0；[XML](result.xml) 为 1 test、0 failures、0 skipped。用例实际断言 fake 观察到 server ID `0xF1234567`、URL `fake://r160_uft_abi`、`confirm=false`，notifier 销毁时对应 ID 的 destroy 调用恰一次。源码中的函数指针类型 `static_assert` 在本次编译通过。

本次没有运行其他测试，也未修改产品代码。fake 模块只证明受控输入下的本机局部调用，不证明真实 MQ provider、可配置 `confirm=true`、Windows/MSVC、其他平台或整体回归；新旧链接库与最终整合树一致性亦未验证。原整项保持未完成。
