# i03_btcore_s5_20261104s5：s5-btcore 红绿夹具

独立 CMake 夹具，用于 BUG-Q02-003（卡3 3d=C 落到 Sel/Hft mocker）与 Q02 集成用例夹具（卡13d 合约/费率）的红绿对照。测试源码直接取自 coverage lane（`q02_btcore3/s5_sel_hft_frozen_tdate_test.cpp`、`q02_btcore3/q02_cta_replayer_integration_test.cpp`），产品源码取自 `WT_SRC_ROOT/WtBtCore/*.cpp`：

- 绿：`WT_SRC_ROOT` 缺省为本仓库 `src`；
- 红：`WT_SRC_ROOT` 指向 `rsync -a --no-links` 得到的源码副本，并用改前产品文件覆盖（不得使用符号链接）。

WTSTools/WTSUtils 由 `WT_LIB_DIR` 提供（红绿应使用同一份，保证只差 WtBtCore 产品源）；这两个静态库若带 `-fprofile-instr-generate` 插桩，保持 `WT_PROFILE_LINK=ON`。

```sh
# 绿
bash src/TestUnits/fixtures/i03_btcore_s5_20261104s5/run.sh green <build dir> <out dir> <WT_LIB_DIR>
# 红
bash src/TestUnits/fixtures/i03_btcore_s5_20261104s5/run.sh red <build dir> <out dir> <WT_LIB_DIR> <red src root>
```

`run.sh` 逐用例独立进程与工作目录运行（120 s 硬超时），写 `results.txt`、各用例日志与 XML。只验证本机 macOS 的受控输入，不代表其他平台、真实行情或整体回归。2026-11-04 结果与哈希见 `docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-btcore/README.md`。
