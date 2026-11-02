# BUG-I02-NEW-043 通知快照和分发定向复验

只读复验，未修改仓库文件或创建符号链接。以当前工作区 `WtCore/WtDtMgr.cpp` 和 `r101_dtmgr_store_test.cpp` 独立重编，用真实实体 Boost bridge `/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include` 优先包含；依赖剩余 WtCore 产品对象和库复用 v8，链接时不包含其他测试对象。具体三条编译/链接命令和对象路径见 `commands.json`，各构建退出 0，日志为 `build-0.log` 至 `build-2.log`。相关产品、头、测试和 fixture 在编译前后及运行后 SHA 一致，见 `source-hashes.json`；二进制 SHA-256 为 `b345d7dc2073bf455e4ecf4181a89dc07f2bef12b9a161fc452c875919d8d57b`。`nm` 在测试可执行文件中确认定义了 `WtDtMgr::on_bar` 和 `on_all_bar_updated`。

真实 WtDataStorageAD 插件从 v8 以普通文件复制到此构建 `bin`，fixture 再复制入该进程私有目录；插件 SHA-256 `e0e332d4dfe2f747300ee362d739ee457da1fe264c17e4e908652ce96094b5b2`。每个单例在独立目录/TMPDIR、30 秒超时运行，命令/退出码见 `results.json`，日志和 XML 在 `run/<case>/`。三项延迟 K 线快照、同步重入分发、异常失败队列保留、回调期间其他线程入队、批次期间其他线程请求 drain 共 7/7 PASS，每例均 1 test / 0 failure / 0 disabled，退出 0。

范围：本机 macOS arm64、fake engine 加真实 AD reader 加载路径；其他产品对象、插件及库来自旧 v8，未证明完整当前工作树一致，也未运行 ASan/TSan 或其他平台。队列锁不证明 reader/重采样/析构和活动回调并发安全，回调抛异常前可能已产生外部副作用，重试不保证恰好一次；第三方引擎异步借用未验。总体回归未运行。
