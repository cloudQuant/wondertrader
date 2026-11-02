# BUG-T4-002 / BUG-I02-NEW-019 direct fake callback retest

Scope: local arm64 fake callbacks only, not real SDK/trading. No repository files changed, no symlinks created. The frozen iteration 03 acceptance wording calls for position conversion and missing-contract nonleak, plus alpha/nonzero IDs, empty/all-zero fallback, and query/push agreement. The original fixture checks two position rows (not just one), with first-row quantities 5/3/1 and retained count 1.

## Build

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-t4-002-019/build.py
```

Build step exits are 0/0/0/0/0. The product TU and original test TU are rebuilt from the current workspace; the additional test TU is an isolated copy of that test file with a query-order snapshot of `entrustID` and two extra tests. Exact compiler/link argument vectors and cwd are in `commands.json`; logs `build-0.log` through `build-4.log`. Ninja provides compile/link settings from `/tmp/wt-i02-fresh-20261101-v8-build`; include/forced header paths are rewritten to the current workspace. Other static/dynamic libraries and target configuration are reused from v8; these are not a fresh full dependency rebuild. Boost include root `/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include` and its `boost` directory are physical directories; `find ... -type l` under that root returned zero. System/prebuilt shared libraries may resolve through their normal system paths.

## Run

```
(cd /tmp/wt-i03-t4-002-019/run-original && LLVM_PROFILE_FILE=/tmp/wt-i03-t4-002-019/run-original/profile.profraw perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-t4-002-019/original --gtest_filter='FemasContractFixture.position_query_response_appends_the_reported_item:FemasContractFixture.position_query_unknown_contract_returns_empty_final_array:FemasContractFixture.make_order_info_maps_vendor_order_fields:FemasContractFixture.make_order_info_falls_back_to_local_id_and_maps_fok:FemasContractFixture.order_query_and_push_callbacks') > /tmp/wt-i03-t4-002-019/original.log 2>&1
(cd /tmp/wt-i03-t4-002-019/run-extra && LLVM_PROFILE_FILE=/tmp/wt-i03-t4-002-019/run-extra/profile.profraw perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-t4-002-019/extra --gtest_filter='FemasContractFixture.isolated_query_push_same_user_order_id:FemasContractFixture.isolated_unterminated_user_order_id_is_bounded') > /tmp/wt-i03-t4-002-019/extra.log 2>&1
```

Original exit 0, 5/5 PASS; extra exit 0, 2/2 PASS. Both filters reported zero skipped selected cases. All other cases, including disabled vendor SDK smoke tests, were not run. Additional query/push test checks `entrustID` equality for the alpha ID and the exact 21-byte non-NUL SDK field test checks bounded output. The direct original test separately checks empty, `0`, `0000`, and `12345` cases. No mutation/red-test run was made; unknown-contract and zero/empty are functional negative inputs, not fault-injection tests.

## Evidence and limits

`source-before.sha256` and `source-after.sha256` match byte-for-byte (cmp exit 0), covering product TU, product header, and original test TU, not all transitive headers. Artifact hashes:

```
9d0ff837936eb23755855f794fb82e4b829a7955384aa70a8941116479175ade  src/TraderFemas/TraderFemas.cpp
f40375e78aa451180e4dae79b932631d36e8fa7ed607ef4edb1b597524e1107d  src/TraderFemas/TraderFemas.h
cfeb6fbbb2796e24e46137bfc0aaed50e3a6fbf733a10e6d4dfc06087dd5ace7  src/TestUnits/coverage/t_TraderFemas/femas_trader_test.cpp
ebcfddf452997c7db50d1635fb273ffe2ce455e10663173210c71cbdad222471  femas_extra.cpp
ecb3f25eaaf2d8bc8cce3bc5507595623a7012d0891a2455f50a96afdea50d36  product.o
60b1db4d935442f076d27058d1eb96139b7223403991c183bea1708bf86a891d  original.o
f69d2ef2eea47a196ff9a0e2fbb6627b7c868b8722be37db4c4e48217d05f346  extra.o
d5c680fa423fec32700bcb4a2c50a7353970f211d5a42e09e2df2181140138ae  original
a2f4f2b9012e52ec60c6c025f6bd29bc6d17921357ded3bebb856566fe089a55  extra
```

No ASan/leak instrumentation was run: the missing-contract source path does not create a position object, and retained-count assertion is limited evidence, not proof of final deallocation or no leak. The full error-terminal partial-fill contract, real SDK callback drain, and T4-003 worker shutdown remain open. No other platform, full regression, or release acceptance is implied.
