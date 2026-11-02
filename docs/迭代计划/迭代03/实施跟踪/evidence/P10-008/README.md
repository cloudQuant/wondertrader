# BUG-P10-008 三种逐笔流同步重入红控

在 macOS 使用已有 v8 `libWtDataStorage.dylib`，通过本目录 `reentry.cpp` 的真实 `createWriter` 接口写入；主代理对 queue/detail/trans 分别运行普通与 sink 同步重入各一次。普通各 exit 0，重入各被进程内部 `alarm(3)` 终止，exit 142；见 results.log 和各日志。writer 源码和旧 v8 快照同文件 SHA 相同，但动态库并非本次当前完整工作树构建，不能外推其他平台或当前树所有依赖。

探针构建命令：

```sh
/usr/bin/clang++ -std=c++17 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I /opt/homebrew/Cellar/boost/1.88.0/include -I /opt/homebrew/Cellar/fmt/11.2.0/include -I src -I src/Includes -I src/Share -I src/WTSTools -I src/WTSUtils -include stdlib.h -include src/Share/fmt_enum_compat.h reentry.cpp -o /tmp/wt-i03-p10-red/reentry
```

运行参数为动态库绝对路径、流类型（queue/detail/trans）、模式（control/reenter）及独立目录。哈希见 `sha256.txt`。测试没有覆盖并发顺序、抛异常、关闭排空或合约生命周期。

**未修复**：块锁内回调确有重入死锁。按块顺序排队还须设计回调对象/合同/商品及 sink 生命周期、同步重入返回时序和关闭排空；现有 DataManager 可能在重入 release 返回后仍删除 writer/卸载动态库，不能把单点移锁称为完整修复。多接收方 void fanout 抛异常后的去重与恢复亦无可证 ACK 合同。旧 ABI 与外部插件兼容方案待落实。
