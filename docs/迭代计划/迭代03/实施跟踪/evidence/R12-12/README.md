# BUG-R12-12 定向复验（macOS arm64）

本次不修改仓库，不创建符号链接。实体 Boost 头为 `/opt/homebrew/Cellar/boost/1.88.0/include`；目录递归检查未发现符号链接。直接编译当前两份测试 TU，并由 TU 编译 `WTSTickData` 和 `CodeHelper` 内联路径；源码和产物散列在 `sha256.txt`。源码文件修改时间早于产物，复验后文件哈希一致。

构建（工作目录仓库根，退出 0；完整 stdout/stderr 为 build.log）：

```sh
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -O1 -fsanitize=address -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/googletest/1.17.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -Isrc -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -include stdlib.h -include src/Share/fmt_enum_compat.h src/TestUnits/coverage/r12_headers/r12_data_def_test.cpp src/TestUnits/coverage/r12_headers/r12_codehelper_test.cpp /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -lpthread -o /tmp/wt-i03-r12-12-check/r12-asan
```

测试均独立进程执行，单例上限 20 秒，每项输出写入各自目录：

```sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 perl -e 'alarm 20; exec @ARGV' -- /tmp/wt-i03-r12-12-check/r12-asan --gtest_filter=r12_tick_data.code_creation_and_checked_setter_enforce_capacity --gtest_output=xml:/tmp/wt-i03-r12-12-check/r12_tick_data.code_creation_and_checked_setter_enforce_capacity/result.xml
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 perl -e 'alarm 20; exec @ARGV' -- /tmp/wt-i03-r12-12-check/r12-asan --gtest_filter=r12_codehelper.std_code_to_hot_and_2nd_wrappers --gtest_output=xml:/tmp/wt-i03-r12-12-check/r12_codehelper.std_code_to_hot_and_2nd_wrappers/result.xml
```

两项均退出 0，XML 各 1/1 PASS，0 disabled、0 skipped，日志见对应目录。第一项覆盖 null、0/1/31 字节、32 字节及更长拒绝、NUL、显式长度和重叠复制、拒绝后旧值保持；第二项覆盖 HOT/2ND 完整字符串、长度 9 及无分隔符正控。实际 case 和退出码在 results.log。

ASan 仅覆盖当前测试 TU 及头内联代码，预编译 GoogleTest/系统依赖未全插桩，LeakSanitizer 关闭；没有重新运行修复前红控。Q04 同步拒单未回滚、NEW-030 的历史 container-overflow，真实供应商、Windows、完整当前树及整体回归均未由本次验证关闭。
