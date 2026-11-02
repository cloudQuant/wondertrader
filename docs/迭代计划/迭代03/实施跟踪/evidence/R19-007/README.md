# BUG-R19-007 命令队列附着定向复验

使用 `/tmp/wt-i03-r58-src` 源码快照和 `/tmp/wt-i03-r58-build` 构建目录，执行 `/Users/yunjinqi/opt/anaconda3/bin/ninja -C /tmp/wt-i03-r58-build CoverageTests_mqshare -j 4`，退出 0。测试目标中的 `ShareBlocks.cpp` 和测试 TU 以实体 `cov_boost_include` 优先编译；二者的快照哈希与运行时工作区相同，见 `sha256.txt`。其余依赖来自同一隔离构建，但部分产品库包含既有系统 Boost 链接路径，且源码快照与现工作区其他文件不一致，不能称完整当前树通过。

分别在隔离目录、有界 20 秒下执行以下三个 `R19TempDir` 单例，均 exit 0、XML 各 1/1 PASS：跨圈队列附着、跨进程冷初始化、跨进程生产者接管及旧游标读取。文件名即单例名称。

仅支持本机同版协作路径的证据；Windows、新旧二进制混用、多写者并发及整体回归未验证。
