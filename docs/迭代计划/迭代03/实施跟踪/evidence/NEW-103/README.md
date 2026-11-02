# BUG-I02-NEW-103 dtcore 用例定向复验

2026-11-01，本机 macOS arm64。冻结条目涉及四个测试 oracle：空共享内存路径、UDP 接收方类型、环形写游标和缺失/短文件日志。

从 `/tmp/wt-i02-fresh-20261101-v8-build` 的 `CoverageTests_dtcore` Ninja 命令提取参数；将 4 个当前工作区测试 TU 和当前 `WtDtCore/ShmCaster.cpp`、`WtDtCore/UDPCaster.cpp`、`WtDtHelper/WtDtHelper.cpp` 单独重编，再替换 v8 对应 7 个对象重链，其余对象、库复用旧 v8。精确 argv 在 `commands.json`，输出在 `build-0.log` 至 `build-7.log`；实体 `cov_boost_include/boost` 被相关含 Boost 编译单元依赖文件引用，输出目录未创建符号链接。产品可执行文件中的 `ShmCaster::init`、`UDPCaster::addBRecver` 和 `resample_bars` 定义用 nm 核对。相关源文件与 header 构建前后及核对时哈希一致，见 `hashes.json`；其余旧对象不证明与当前全树一致。

四项在独立工作目录与 TMPDIR 中逐例运行，各 exit 0/1 PASS，无跳过：

- `R145DtCoreEdgesTest.ShmCasterEmptyPathReturnsFalseAndKeepsUninitializedGuards`；
- `R164UdpCasterActiveTest.RegistrationBucketsAndRejectInvalid`；
- `R18ShmCasterTest.RingIndexModuloWrapsWithinSmallQueue`；
- `Q05ResampleTest.MissingFileReadErrorAndShortFileFailsHeadCheck`。

四项合并同进程关联运行 4/4 PASS，无跳过；所有命令、退出码、XML 属性在 `results.json`，日志和 XML 在 `run0` 至 `run4`。`R18` 用本地小数组复制环形下标逻辑，并不调用真实共享映射写路径；`R164` 的组播注册依赖本地网卡，不发送数据。

本结果只覆盖上述修正 oracle，不证明日线 `times=1` 和 DataManager 初始化失败态的合同，也不证明多线程、真实发送端或跨平台行为。未运行 sanitizer、完整当前树构建或整体回归，本轮无旧版本红控。仓库文件未修改。
