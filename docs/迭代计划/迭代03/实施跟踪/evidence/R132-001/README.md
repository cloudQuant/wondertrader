# BUG-R132-001 本机 ASan 定向复验

使用独立 `/tmp/wt-i03-udp/source/src` 快照和 `/tmp/wt-i03-r132/build` 输出，重编 `WtDataReader.cpp` 及 `r132_reader_his_test.cpp`；相关产品和测试编译单元均使用 LLVM 22、`-O1 -fsanitize=address -fno-omit-frame-pointer`。Boost 包含路径优先使用实体 `/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include`，两份依赖文件均显示该目录的 `boost/version.hpp`。具体编译和链接参数保存在 `commands.json`。

其余测试对象和依赖库只读复用旧 v8 构建，未做全依赖 ASan 插桩；链接输出含 DWARF 解析警告但退出 0。可执行文件 `nm` 显示三个 `WtDataReader::readOrdQueSlice`、`readOrdDtlSlice`、`readTransSlice` 定义位于可执行文件的 `__TEXT`，reader.o 引用 ASan 检查符号，见 `product-symbols.txt` 与 `reader-asan-symbols.txt`。`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`，无 LSan。

四个单例独立进程执行，单例上限 20 秒：`R132ReaderHisHftCompressedRoundTrip`、`R132ReaderHisCompressibleQueueExpansion`、`R132ReaderHisOrderQueueDetailAndTransCachesAreIndependent`、`R132ReaderHisMalformedCompressedRecordsAreRejected`；各自 exit 0、XML 1/1 PASS、0 disabled。覆盖 queue/detail/trans 高压缩首读和缓存复读、字段哨兵及损坏输入；本次无 ASan 报告，是否启用标准库容器内部注解未另验证。

快照与当前工作区相关 reader/test/fixture 与数据头在运行前后 SHA 一致，见 `hashes-before.json`、`hashes-after.json`；未证明仓库其他所有文件一致。二进制哈希见 `binary-sha256.txt`，动态库见 `dynamic-dependencies.txt`。首次 reader 编译缺少 RapidJSON include，保留 `build/compile-reader-initial-failure.log`；后加 `-I/usr/local/include` 成功，实体 Boost 目录仍位于其前。未创建符号链接，未运行其他整套测试。CMP_V2 解压资源预算与拒绝策略、Windows/Linux、最终当前树集成回归仍未验证。
