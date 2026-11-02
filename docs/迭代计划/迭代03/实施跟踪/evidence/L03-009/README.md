# BUG-L03-009 safety sub-slice (INCOMPLETE)

Before change, UBSan probe with `offset=INT32_MAX` and time 0900 exited 134 due to signed addition overflow. This is `red.cpp`, `red-max.log`; it was compiled against the then-current header before the fix:

```
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -O1 -fsanitize=undefined,signed-integer-overflow -fno-sanitize-recover=undefined -I/opt/homebrew/Cellar/boost/1.88.0/include -Isrc /tmp/wt-i03-l03-009/red.cpp -o /tmp/wt-i03-l03-009/probe-red
```

After change, `l03_session_info_test.cpp` was compiled as follows and run with the listed filter:

```
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -O1 -fsanitize=undefined,signed-integer-overflow -fno-sanitize-recover=undefined -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/googletest/1.17.0/include -Isrc src/TestUnits/coverage/l03_includes/l03_session_info_test.cpp /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -lpthread -o /tmp/wt-i03-l03-009/l03-ubsan
/tmp/wt-i03-l03-009/l03-ubsan --gtest_filter='l03_session.offset_arithmetic_keeps_valid_offsets_and_avoids_int32_overflow:l03_session.offset_time_zero_offset:l03_session.offset_time_positive_offset_wrap:l03_session.offset_time_negative_offset_wrap:l03_session.original_time_inverse_mapping:l03_session.offset_date_branches' --gtest_output=xml:/tmp/wt-i03-l03-009/focused.xml
```

6/6 PASS, no skips; `focused.log` and `focused.xml` record results. Test TU / inline product code is instrumented; GTest is prebuilt. `sha256.txt` records source and binary hashes.

The change only widens intermediate arithmetic and preserves existing single-day rollover. Offset validity, multi-day time/date consistency, public create/setter failure semantics and config transactional publication remain undetermined. Extreme values can still produce semantically invalid times, and no supported range is assumed. This is not acceptance of BUG-L03-009; no Windows or full regression was run.
