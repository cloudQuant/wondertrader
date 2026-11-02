# BUG-L05-002 close 失败传播（候选，未验收）

`BoostFile::close_file()` 保留原 void 接口；新增 `close_file_checked()` 在报告底层返回值前先使句柄无效，不在 POSIX 上重试失败的 close，避免错误时潜在重复关闭。`write_block_file` 仅在写入和关闭均成功后报告成功；Windows 的原生 `WriteFile` 还需核对实际写入字节数，拒绝无法表示为 DWORD 的长度。该代码直接使用系统无用户态缓冲文件写入，未调用 fsync，不能据此声称断电持久性。

构建，cwd 仓库根目录（首次未带 profile runtime 的链接失败，`build.log`；以下成功）：

```sh
/usr/bin/clang++ -std=c++17 -g -O1 -fprofile-instr-generate -fcoverage-mapping -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DWT_DT_HELPER_CLOSE_TEST_HOOKS -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/googletest/1.17.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -Isrc -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -include stdlib.h -include src/Share/fmt_enum_compat.h src/WtDtHelper/WtDtHelper.cpp src/TestUnits/coverage/l05_dt_servo_porter/l05_dt_helper_bar_test.cpp /tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSTools.a /tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSUtils.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -L/opt/homebrew/lib -L/usr/local/lib -lboost_filesystem -lboost_thread -lzstd -liconv -ldl -lpthread -o /tmp/wt-i03-l05-close/l05-test
```

该命令另加 `-Wall -Wextra -Werror -Wno-deprecated-copy-with-user-provided-copy -Wno-\#pragma-messages` 构建 `l05-test-werror` 也成功。未定义测试 seam 的产品 `-fsyntax-only`、严格告警成功。两个编译单元来自当前工作区，其他静态库复用 `/tmp/wt-i03-r58-build` 快照。

六个定向测试均逐进程 `LLVM_PROFILE_FILE=/tmp/wt-i03-l05-close/<case>.profraw perl -e 'alarm 20; exec @ARGV' -- /tmp/wt-i03-l05-close/l05-test --gtest_filter=L05HelperBarTest.<case> --gtest_output=xml:/tmp/wt-i03-l05-close/<case>.xml` 退出 0：

- `CheckedCloseIsIdempotentAfterClosingRealFile`
- `CheckedCloseReportsNativeFailureWithoutSecondClose`：测试显式关闭 fd 触发真实 EBADF；验证错误返回、句柄清空及后续不会关闭重用 fd，不代表磁盘故障。
- `StoreBarsReportsInjectedCloseFailureWithoutSuccess`：测试 seam 先真实 close 再注入失败返回，验证 false 和失败日志，非真实系统 close 错误。
- `StoreBarsReportsWriteFailure`：用真实 RLIMIT_FSIZE 写入失败控制。
- `StoreBarsReportsMissingParentDirectoryFailure`
- `StoreAndReadBarsRoundtrip`

R16 兼容控制：使用实体 Boost 头单独编译当前 `r16_ini_boostfile_test.cpp`，执行 `r16_boostfile.sparse_file_offsets_above_4gib_preserve_size_seek_and_sentinels:read_file_exact_length_true_short_read_false:read_exact_chunking_preserves_lengths_above_4gib:static_write_and_read_file_contents_roundtrip`（完整名称见 `r16-focused.log`），4/4 PASS，无 skip；稀疏逻辑大小 4294975488，分配量 0/8192。分块大长度测试是模拟回调，并非读取整段 4 GiB。

当前源码和二进制散列见 sha256.txt。未实测 Windows 短写或 CloseHandle 失败、真实存储故障/断电、全依赖插桩或最新完整树；新增方法未改变对象布局或旧接口签名，但 Windows ABI 未实测。`dump_bars/dump_ticks` 无返回值的成功日志问题未改。本次不声明整体回归或生产验收。
