# R132-001 隔离一致性定向复验：发现历史记录未对齐

本次基于 `ccaad010` 的 `src` 实体快照，隔离构建并运行 R132 四个相关测试，未更改仓库产品或测试代码。产品 reader、测试、WTSTools 和 WTSUtils C++ 代码使用同一 LLVM 22 工具链和 ASan/UBSan 选项；GTest、Boost、系统库为预编译依赖，WTSUtils 的 C 源没有通过 CXX_FLAGS 插桩，因而不构成全依赖插桩。Boost 1.88 头使用实体 Cellar 路径且 coverage 会实体复制；源、构建、运行目录的 `find ... -type l` 无结果。关键源码及二进制哈希见 `source-binary.sha256`，动态依赖见 `dynamic-dependencies.txt`。

## 命令与构建边界

```sh
mkdir -p /tmp/wt-i03-r132-20261102/source
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r132-20261102/source
# 对隔离快照的 CMakeLists 应用本目录 cmake-focus.patch，未修改仓库 CMake。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
 -S /tmp/wt-i03-r132-20261102/source/src -B /tmp/wt-i03-r132-20261102/build -G Ninja \
 -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON \
 -DCMAKE_C_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang \
 -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ \
 -DCMAKE_CXX_FLAGS='-include stdlib.h -fsanitize=address,undefined -fno-omit-frame-pointer' \
 -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
 -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined' \
 -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
 -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r132-20261102/build --target R132Focused -j 6
```

实际最后构建成功，完整 TU 命令见 `build-commands.log`；`cmake-focus.patch` 描述隔离快照的测试目标裁剪及独立最小链接目标。第一次未加 `-include stdlib.h`，LLVM 22 预包含 fmt 时出现 malloc/free 未声明，构建 exit 1，见 `build.log`；加预包含后的 CoverageTests 仍因为无关 `HftMocker.h` 的 `-Werror=nontrivial-memcall` 失败（`rebuild.log`）。复制保留 `refocus-build.log`，改用独立最小链接目标才构建成功（`minimal-build.log`）；不能称完整 CoverageTests 目标构建通过。

## 逐项结果

四个测试分别单独进程运行，使用 `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，命令模板：

```sh
R132Focused --gtest_filter=R132ReaderHisTest.TEST_NAME --gtest_output=xml:TEST_NAME.xml
```

- `R132ReaderHisHftCompressedRoundTrip`：exit 134，UBSan 报 `WTSOrdQueStruct` 地址不满足 8 字节对齐。
- `R132ReaderHisCompressibleQueueExpansion`：exit 134，同样报告 `WTSOrdQueStruct` 未对齐。
- `R132ReaderHisOrderQueueDetailAndTransCachesAreIndependent`：exit 134，先报告 `WTSOrdDtlStruct` 未对齐；进程在最先发生的 UB 中止，未证明其后成交分支正常。
- `R132ReaderHisMalformedCompressedRecordsAreRejected`：exit 0、1/1 PASS（XML 已保存）。

各原始日志保存在本目录，失败没有有效完成 XML，不能计 PASS。没有运行整体回归；没有 LSan 结论。

## 根因与所有权静态分析

`DataDefine.h` 的历史块以 `#pragma pack(1)` 记录，RAW 头 12 字节；解压后的 bytes 被拼为 `BlockHeader + payload` 并保存在 `His*BlockPair::_buffer`（std::string）。`WtDataReader.cpp` 把 12 字节后数据直接作为对齐要求为 8 的 typed 数组传入 `std::lower_bound`，UBSan 在此确认了未对齐引用。`WTSOrdQueSlice`、`WTSOrdDtlSlice` 和 `WTSTransSlice` 的 create 只保存传入的裸记录指针；slice 的 retain 只保留 slice 对象，并不保留 reader 中的 buffer。即便只把比较器改为 memcpy，也仍会向外返回未对齐的借用指针，reader 销毁后 slice 会失去底层存储。

候选方向是解码时逐条复制到对齐的 typed 存储，并以明确的共享 owner 或新版本化拥有型 slice 保证生命周期；需审查 C++ 对象构造、slice/插件 ABI、缓存替换与并发。不能更改现有磁盘块布局。NEW-067/120 涉及另一个 `WtRdmDtReader` 的 clearCache，但它共享相同裸借用 slice 设计，应协同制定缓存清理、存活 slice 和并发读取合同；不能将 R132 本机测试外推为其实现已修复。

压缩帧资源预算与拒绝策略、Windows/Linux、真实数据规模、reader 及 slice 生命周期、整合回归均未验证；本条保持未完成。

## 历史 HFT 对齐局部修复

按后续授权，在 `WtDataReader.cpp/.h` 的三个历史 HFT 缓存中，将已解码 payload 校验后复制到 reader 拥有的 `std::vector<T>`，再用该连续、对齐的记录数组进行 lower_bound 和构造原有借用 slice；不修改磁盘块、公开 slice 签名或返回端点。记录数在解压前检查不超过 `UINT32_MAX`，复制前检查元素整除、`vector::max_size()` 和可平凡复制；新 vector 分配异常转换为失败，未发布半成品。解压后原有临时 raw 字节与 vector 会短时同时存在；无输入或峰值预算，不构成资源有界证明。现有整文件读取及 map 插入可能抛异常，未由本次局部改动解决。

新增检查三类 slice 的记录首址对齐和连续布局、精确结束时间的排他语义；额外持有 slice 并插入 128 个其他缓存项，验证一次 map 扩容后的存活。修复测试 fixture 中把随机 double 赋给 uint32 volume 的 UB，改为随机 uint32；中间阶段该 fixture UBSan 失败日志为 `fix-R132ReaderHisHftCompressedRoundTrip.log`，不是产品对齐复验通过或失败的替代。

最终测试源码配旧产品头和源码重编后，对原有四项及新增端点用例运行：除损坏输入控制 exit 0，四项均在旧版未对齐处 exit 134，日志名以 `red-` 开头。重编当前三个相关源码后，以前述相同工具链与 ASan/UBSan 条件逐进程运行五项，再加“其他缓存插入”和“空记录”用例，共 **7/7 exit 0**，各自 `last-*.log` 和 XML 保存在此。旧版红控未包含后新增的缓存插入用例，不能据此推定其旧版结果。当前源码和快照的这三个文件逐一 `cmp` 相同，哈希及可执行文件哈希见 `final-source-binary.sha256`；`git -c core.whitespace=cr-at-eol diff --check` 针对三个修改文件通过。

本机结果仅覆盖历史 queue/detail/trans，测试及 reader、WTSTools/WTSUtils C++ 使用同工具链 sanitizer，但外部库和 C 对象未全部插桩，LSan 未运行。`WtDataReader` 类布局变化，外部直接实例化二进制需共同重编；未证明插件 ABI 混用。slice 仍借用 reader 缓存，持有 slice 期间 reader 不得销毁；本次未建立并发读取/销毁协议，未处理 WtRdmDtReader 的清理，不能关闭 NEW-067/120。真实内存分配失败、峰值预算和拒绝策略、其他平台与整体回归尚未验证。

## 历史 HFT 缓存的事务式发布

主代理复核发现：对齐版本先用 `map[key]` 占位，损坏输入或分配失败后，该键永久命中未就绪条目，文件修好也不能重读。本次修改三个历史 HFT 路径：读取、解压和 aligned vector 在局部 candidate 中完成，完成后才 `try_emplace`；读入、验证、分配、发布失败均不留下占位，允许后续重试。保留既有时间切片语义、成功缓存的 slice 裸借用、磁盘格式及三个不同类型的缓存隔离。

历史 map 由 `wt_hashmap` 改为 `std::unordered_map`，仅限 WtDataReader 的三个 HFT map。仓库版本的 `ankerl::unordered_dense::increase_size` 先释放旧 buckets 再分配新 buckets，故不能单独以先准备 candidate 推断其增长失败时的 map 完整性；标准容器节点不因 rehash 移动，已返回记录存储不因成功插入其他键迁移。内存、吞吐、外部直接实例化 ABI 和并发读写未测试，相关代码需要一起重编；不能外推到其他 map。

新增 `R132HistoricalBadFilesCanBeReplacedAndRetried`：三个路径先读损坏短文件，再覆盖为有效文件并用同一 reader 同键重读。使用对齐修复但尚未事务发布的源码作为红控，三个路径的重试断言均失败、exit 1，见 `txn-red.log`/`.xml`。红控源码因最终测试文件包含测试钩子引用，仅在隔离快照末尾添加无操作的钩子桩以完成链接；本红控未调用该桩。红控对应源码来自本事务修改前的本地快照，而非原始 Git HEAD。

新增的测试专用线程局部钩子仅在 `WT_RDM_DT_READER_TEST_HOOKS` 下编入；分别在创建 aligned vector 之前与调用 map 插入之前抛出 `std::bad_alloc`，对三类路径各执行失败、保留已有 slice、同键重试正控。该注入不模拟真实 allocator 在容器 rehash 内部失败，不能作为真实内存耗尽验证或 stdlib 实现独立认证。

最终三个工作区源文件与隔离快照逐一 `cmp` 一致；同一 LLVM 22 和前述 ASan/UBSan 设置下相关 9 个测试分别运行均 exit 0、9/9 PASS，证据 `txn-final-*.log` 和 `.xml`，构建为 `transaction-final-build.log`，哈希 `transaction-source-binary.sha256`。`git -c core.whitespace=cr-at-eol diff --check` 对三个源文件退出 0。真实 OOM、内存预算、其他平台、TSan、reader 与 slice 销毁并发、其它 reader 和最终整体回归均未验证。
