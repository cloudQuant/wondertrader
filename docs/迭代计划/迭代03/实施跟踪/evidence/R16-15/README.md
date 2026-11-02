# BUG-R16-15 定向复验

执行目录：/Users/yunjinqi/Documents/new_projects/wondertrader；macOS arm64。源码、测试和产物 SHA-256 在 sha256.txt；Boost 头直接使用 `/opt/homebrew/Cellar/boost/1.88.0/include` 的实体目录，未创建符号链接。测试 TU 编译时包含当前工作区 BoostFile.hpp；这是单 TU 独立构建，不代表完整工作树已构建。

稀疏能力预检：使用 Python 对 `/private/tmp/wt-i03-r16-check/preflight.bin` 临时文件 truncate 至 33554432 字节并 fstat，结果逻辑 33554432、物理分配 0；该临时预检文件已删除。

构建（exit 0，build.log 空）：

```sh
/usr/bin/clang++ -std=c++17 -g -O1 -Wall -Wextra -Werror -Wno-deprecated-copy-with-user-provided-copy -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/googletest/1.17.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -Isrc src/TestUnits/coverage/r16_share_tools/r16_ini_boostfile_test.cpp /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -lpthread -o /private/tmp/wt-i03-r16-check/r16-test
```

稀疏用例（exit 0，1/1 PASS，无跳过）：

```sh
TMPDIR=/private/tmp/wt-i03-r16-check perl -e 'alarm 30; exec @ARGV' -- /private/tmp/wt-i03-r16-check/r16-test '--gtest_filter=r16_boostfile.sparse_file_offsets_above_4gib_preserve_size_seek_and_sentinels' '--gtest_output=xml:/private/tmp/wt-i03-r16-check/sparse.xml'
```

sparse.xml 记录逻辑 4294975488 字节，写入前分配 0 字节，写入哨兵后分配 8192 字节；跨 4 GiB 和高偏移 sentinel 均验证。这是 sparse offsets 和小规模读写，不是将整个 4 GiB 文件读入内存。

其它直接关联用例（exit 0，6/6 PASS，无跳过）：

```sh
TMPDIR=/private/tmp/wt-i03-r16-check perl -e 'alarm 30; exec @ARGV' -- /private/tmp/wt-i03-r16-check/r16-test '--gtest_filter=r16_boostfile.read_file_exact_length_true_short_read_false:r16_boostfile.read_exact_chunking_preserves_lengths_above_4gib:r16_boostfile.read_exact_chunking_retries_short_reads_and_rejects_eof:r16_boostfile.static_write_and_read_file_contents_roundtrip:r16_boostfile.static_read_file_contents_preserves_binary_bytes:r16_boostfile.read_file_contents_false_for_missing_or_empty' '--gtest_output=xml:/private/tmp/wt-i03-r16-check/focused.xml'
```

`read_exact_chunking_preserves_lengths_above_4gib` 使用模拟 callback 证明大长度分块，不证明实际读取 4 GiB 内容；Windows、其他平台、完整树构建和整体回归未运行。
