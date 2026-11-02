# BUG-I02-NEW-085 定向复验

独立编译当前 `r1_datadef_slices_test.cpp`，直接使用实体 Boost/GTest/fmt 头；GTest 静态库为本机预编译版本，未链接旧工程对象或库。下列输入源码在编译前后的 SHA-256 一致：`src/Includes/WTSDataDef.hpp`、`src/Includes/WTSCollection.hpp`、`src/TestUnits/coverage/r1_storage/r1_datadef_slices_test.cpp` 和 `src/TestUnits/coverage/q03_storage3/q03_storage3_fixture.h`，见 input-before.sha256 / input-after.sha256；产物见 binary.sha256。未创建符号链接。

构建（exit 0）：

```sh
/usr/bin/c++ -std=c++17 -g -O1 -Wall -Wextra -Werror -Wno-deprecated-copy-with-user-provided-copy -Wno-\#pragma-messages -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/googletest/1.17.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -Isrc -include src/Share/fmt_enum_compat.h src/TestUnits/coverage/r1_storage/r1_datadef_slices_test.cpp /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -lpthread -o /private/tmp/wt-i03-new-085/focused
```

分独立目录，以 `TMPDIR=<用例目录> perl -e 'alarm 20; exec @ARGV' -- /private/tmp/wt-i03-new-085/focused --gtest_filter=R1DataDefTest.<用例名> --gtest_output=xml:<用例目录>/result.xml` 各执行一次：

- `kline_data_flags_period_and_range_bounds`：exit 0，1/1 PASS；
- `kline_slice_extract_data_all_field_types`：exit 0，1/1 PASS。

原始输出及 XML 在同名子目录；无 skip。仅 macOS arm64、本机独立单 TU；未覆盖原计划中订单明细越界合同，也未运行跨平台、完整工作树构建或整体回归。
