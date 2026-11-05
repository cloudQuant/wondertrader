# I03 HFT / SEL strategy ID 功能检查

从仓库根目录执行（独立输出目录，不修改构建树）：

```sh
src/TestUnits/fixtures/i03_strategy_id_20261103/run.sh /tmp/wt_i03_strategy_id_build
```

脚本按 HFT、SEL 顺序编译独立插件与检查程序，并分别运行；单个编译进程串行执行（不超过 `-j2`）。需要 C++17、pthread、Perl，以及 Boost、fmt 和 spdlog 头文件。默认使用本机 Homebrew 路径，可通过 `CXX`、`WT_BOOST_INCLUDE`、`WT_FMT_INCLUDE`、`WT_SPDLOG_INCLUDE` 覆盖。macOS 使用 `.dylib`，其他平台使用 `.so` 和 `-ldl`。每个检查设置 20 秒超时。

检查真实管理器及本地测试插件：两个创建重载、重复 ID 不再次调用 creator 且保留原对象、不同 ID、creator 返回空或抛异常后的同 ID 重试、creator 内同步重入，以及另一个线程正在创建时拒绝同 ID、允许不同 ID。`getStrategy` 对尚未发布的占位返回空。

`evidence-20261103/current.log` 是上述脚本在 `/tmp/wt_i03_strategy_id_evidence_20261103` 的输出；`source.sha256` 是对应的源文件哈希（仓库根目录相对路径），`binaries.sha256` 是该次本机隔离构建的产物哈希（绝对路径）。核对源码可执行：

```sh
shasum -a 256 -c src/TestUnits/fixtures/i03_strategy_id_20261103/evidence-20261103/source.sha256
```

`hft-baseline.log` 和 `sel-baseline.log` 是修改前源码快照下同一功能检查的输出，两者均以状态码 1 退出，显示重复创建等检查失败。快照原始哈希见 `baseline-source.sha256`，快照当时保存在 `/tmp/wt_i03_strategy_id_before_20261103`；该临时目录不保证长期存在，因此这些日志不是可独立重建的基线包。

`legacy-controls.log` 记录了单独编译旧 HFT、SEL factory fixture 后，仅运行的三个正常控制：HFT `owner-null-strategy`、`owner-outlives-manager`，SEL `owner-no-preload`。`legacy-source.sha256` 记录对应 fixture 源文件。旧 fixture 的空返回控制现在检查不发布空 wrapper 并验证同 ID 成功重试；其他异常、崩溃、UAF 和历史反例未运行。

这三个控制在本机 macOS 上的构建和执行命令如下；每次使用新的临时目录以避免复用标记文件：

```sh
src="$PWD/src"
b=$(mktemp -d /tmp/wt_i03_legacy.XXXXXXXX)
for kind in Hft Sel; do
  if [ "$kind" = Hft ]; then fixture="$src/TestUnits/fixtures/hft_factory_quarantine"; else fixture="$src/TestUnits/fixtures/sel_factory_quarantine"; fi
  mkdir -p "$b/$kind/plugins"
  c++ -std=c++17 -pthread -fPIC -dynamiclib "$fixture/factory.cpp" -o "$b/$kind/plugins/fixture.dylib"
  c++ -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
    -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include \
    -I/opt/homebrew/Cellar/spdlog/1.15.3/include -include "$src/Share/fmt_enum_compat.h" \
    "$src/WtCore/${kind}StrategyMgr.cpp" "$fixture/logger_stub.cpp" "$fixture/check.cpp" -o "$b/$kind/check"
done
WT_HFT_MARKER="$b/hft-null.marker" perl -e 'alarm 20; exec @ARGV' -- "$b/Hft/check" owner-null-strategy "$b/Hft/plugins" "$b/Hft/plugins/fixture.dylib"
WT_HFT_MARKER="$b/hft-owner.marker" perl -e 'alarm 20; exec @ARGV' -- "$b/Hft/check" owner-outlives-manager "$b/Hft/plugins" "$b/Hft/plugins/fixture.dylib"
WT_SEL_MARKER="$b/sel-owner.marker" perl -e 'alarm 20; exec @ARGV' -- "$b/Sel/check" owner-no-preload "$b/Sel/plugins" "$b/Sel/plugins/fixture.dylib"
```

该检查不覆盖管理器销毁与并发调用交错、所有工厂生命周期路径、完整运行器启动链、全量回归、真实交易或线上 provider。SEL 现有工厂保留策略不由本 fixture 验证或改变。
