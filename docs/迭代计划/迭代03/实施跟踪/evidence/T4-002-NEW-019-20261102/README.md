# T4-002 / NEW-019 本机定向复核，2026-11-02

以当前工作区的 TraderFemas.cpp、头文件和现有 fake fixture 构建，源码哈希见 sources.sha256。运行机器为 macOS arm64，Apple clang 17，Boost 1.88，Google Test 使用 Homebrew 预编译静态库。Boost 头由 `/opt/homebrew/Cellar/boost/1.88.0/include/boost` 复制为 `/tmp/wt-i03-femas-20261102/include/boost`，`find ... -type l` 无输出；没有创建符号链接。此构建为局部源测试，使用动态符号解析，不是全树或真实 SDK 构建。

复现命令在 `sh` 中执行，工作目录为仓库根目录，目标目录为 `/tmp/wt-i03-femas-20261102`，独立运行目录为其 `run` 子目录：

```sh
mkdir -p /tmp/wt-i03-femas-20261102/include /tmp/wt-i03-femas-20261102/run
cp -R /opt/homebrew/Cellar/boost/1.88.0/include/boost /tmp/wt-i03-femas-20261102/include/
find /tmp/wt-i03-femas-20261102/include -type l -print

# 以下公共选项对产品和测试分别编译。
COMMON='-std=gnu++17 -g -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/tmp/wt-i03-femas-20261102/include -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -isystem /opt/homebrew/include -isystem src -include src/Share/fmt_enum_compat.h'
/usr/bin/c++ $COMMON -Dinst_hlp=femas_product -c src/TraderFemas/TraderFemas.cpp -o /tmp/wt-i03-femas-20261102/product-normal.o
/usr/bin/c++ $COMMON -DWT_T4_FEMAS_MISSING_LIBRARY_MODULE='"wt_t4_femas_module_absent"' -DWT_T4_FEMAS_MISSING_SYMBOL_MODULE='"wt_t4_femas_missing_symbol"' -DWT_T4_FEMAS_NULL_CREATOR_MODULE='"wt_t4_femas_null_creator"' -DWT_T4_FEMAS_SUCCESSFUL_CREATOR_MODULE='"wt_t4_femas_successful_creator"' -Dinst_hlp=femas_test -c src/TestUnits/coverage/t_TraderFemas/femas_trader_test.cpp -o /tmp/wt-i03-femas-20261102/test-normal.o
/usr/bin/c++ -g /tmp/wt-i03-femas-20261102/product-normal.o /tmp/wt-i03-femas-20261102/test-normal.o -o /tmp/wt-i03-femas-20261102/test-normal -Wl,-undefined,dynamic_lookup -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -lboost_filesystem -lboost_thread -liconv
cd /tmp/wt-i03-femas-20261102/run
perl -e 'alarm 30; exec @ARGV' -- ../test-normal --gtest_filter='FemasContractFixture.position_query_response_appends_the_reported_item:FemasContractFixture.position_query_unknown_contract_returns_empty_final_array:FemasContractFixture.make_order_info_maps_vendor_order_fields:FemasContractFixture.make_order_info_falls_back_to_local_id_and_maps_fok:FemasContractFixture.order_query_and_push_callbacks'
```

上述编译、链接、运行退出 0；5/5 PASS，详见 `run-normal.log`。两条持仓用例覆盖有效两帧方向/数量、回调时各对象引用数为 1 以及未知合约的空结果；这不证明最终释放或不存在泄漏。

额外控制使用上轮 `../T4-002-NEW-019/femas_extra.cpp`（即原 fixture 的查询 ID 快照和两条追加测试），以相同公共选项、四个相同 `-DWT_T4_...` 宏以及 `-Dinst_hlp=femas_extra` 编译得到 `extra-normal.o`，替换上面链接中的 `test-normal.o`，输出 `extra-normal`，并运行：

```sh
perl -e 'alarm 30; exec @ARGV' -- ../extra-normal --gtest_filter='FemasContractFixture.isolated_query_push_same_user_order_id:FemasContractFixture.isolated_unterminated_user_order_id_is_bounded'
```

编译、链接、运行退出 0，2/2 PASS，见 `extra-run.log`。这是 fake callback 测试，不证明实际 SDK 生命周期或错误终态。

相同产品和原测试编译命令加 `-O1 -fsanitize=address,undefined -fno-omit-frame-pointer` 并链接同样 sanitizer，使用 `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` 与上述 5 项过滤器运行；编译链接均退出 0，但 30 秒无输出被 alarm 终止，退出 142，不是 sanitizer 通过。用 `asan-smoke.cpp` 编译相同 sanitizer 的最小程序并运行 `ASAN_OPTIONS=detect_leaks=0 perl -e 'alarm 10; exec @ARGV' -- .../asan-smoke`，也无输出、退出 142。工具不可用范围不能由此断言原因。

`perl -e 'alarm 30; exec @ARGV' -- /usr/bin/leaks --atExit -- ../test-normal --gtest_filter='FemasContractFixture.position_query_response_appends_the_reported_item:FemasContractFixture.position_query_unknown_contract_returns_empty_final_array'` 报告两测试通过及 0 leaks，但提示目标不可调试。用 `leak-control.cpp` 构造明确分配后不释放的正控，同样报告 0 leaks；因此此 leaks 结果不能证明无泄漏。详细输出分别见 `leaks-run.log` 和 `leak-control.log`。

未修改产品或测试源码；没有进行完整回归。真实 Femas、跨平台、未完成查询/回调排空、错误终态，以及有效的内存工具证据仍未验证。
