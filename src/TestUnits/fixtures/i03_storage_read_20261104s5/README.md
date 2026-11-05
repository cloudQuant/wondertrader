# 迭代03 s5 storage-read 红绿夹具（2026-11-04）

覆盖 BUG-L04-004、BUG-R100-003/BUG-R32-003（读端）、BUG-R122-002（有界暂存/损坏/只读目录）、R21-BUG-07（快照复用）以及卡15 Z 类复跑。测试本体在 coverage lane `p05_datastorage2/s5_*_test.cpp`（同时进入 `CoverageTests_adstorage`），本夹具只负责用指定源码根做 ASan/UBSan 红绿构建。

目标：

- `s5_ad_read`：AD 三个读端 + 写端 TU，加 `s5_reader_ad_bounds_test.cpp`、`s5_rdm_ad_snapshot_test.cpp`、`r21_rdmbars_kline_test.cpp`。
- `s5_ad_lane`：整条 `p05_datastorage2` lane（相关回归）。lane 里既有打包缓存项夹具会先触发 UBSan alignment，需 `-DWT_SAN_ALIGNMENT=OFF`。
- `s5_storage_z`：WtDataStorage 读端 + `q03` 的 tail fragment / R162 / R193 用例（NEW-032/082/059/060 复跑，本包不改这些产品文件）。

```sh
# 1) 普通依赖库（同一编译器）
cmake -S src -B /tmp/wt-i03-s5-storage-read-core -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ "-DCMAKE_CXX_FLAGS=-include cstdlib" \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DWT_WARNINGS_AS_ERRORS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja -C /tmp/wt-i03-s5-storage-read-core -j 2 WTSUtils WTSTools
# 2) 红源码根：工作区 Includes/Share/WTSUtils/WTSTools/FasterLibs/WtDataStorage(AD)/p05 lane 以 rsync --no-links
#    复制，再把改前的读端/键文件（证据目录 before/）覆盖进去
# 3) 绿/红构建与运行
src/TestUnits/fixtures/i03_storage_read_20261104s5/run.sh /tmp/wt-i03-s5-storage-read-core/build_x64/Debug/libs <out> [<red src root>]
```

结果与限制见 `docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-storage-read/README.md`。
