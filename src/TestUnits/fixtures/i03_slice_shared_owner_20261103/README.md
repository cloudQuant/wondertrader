# 迭代03 卡11=B：历史切片共享 owner（2026-11-03 裁定，2026-11-04 完成本地定向验证）

## 设计

- `WTSDataDef.hpp`：五类切片（`WTSKlineSlice`/`WTSTickSlice`/`WTSOrdDtlSlice`/`WTSOrdQueSlice`/`WTSTransSlice`）在**已有成员之后追加** `WTSSliceOwners` 成员，并新增 `addOwner()`/`ownerCount()`；`WTSSliceOwnerPtr = std::shared_ptr<const void>`，`makeSliceOwner(WTSObject*)` 把 WTSObject 包装为 owner（retain/release）。新增写时复制容器 `WTSSharedBuffer<Container>`（vector/circular_buffer 通用）：缓冲区被切片共享时，结构性修改先分离到新缓冲区。`WTSHisTickData` 追加只读 `factor()`。
- 生产者：缓存只释放自己那份引用，切片持有缓冲区 owner。
  - `WtRdmDtReader`：四类历史块改为 `shared_ptr` 槽位；实时切片持有 mmap `_file`；K 线 `_bars/_rt_bars` 改为写时复制；`clearCache` 补清四类历史 map；新增缓存递归锁覆盖全部公开读接口、`clearCache` 与巡检线程；巡检线程改用条件变量可即时停止。
  - `WtDataReader`：实时 tick/HFT/K 线切片持有 mmap owner（重映射后旧切片仍有效）。
  - `WtRdmDtReaderAD`/`WtDataReaderAD`：缓存缓冲区改为写时复制并挂 owner；原“原地追加不重分配”改为整体替换缓冲区（内容相同）。
  - `WtDtServo/WtDataManager`：切片持有缓存 `WTSKlineData`；增量更新前写时复制；`clear_cache` 先清自有重采样缓存与实时 K 线 map 再清 reader；析构依次释放缓存、实时 map，再经 remover 销毁 reader（不卸载模块）；`update_bars` 在锁内判空。
  - `HisDataReplayer`：`HftDataList::_items` 改为写时复制，tick/逐笔切片持有缓冲区 owner，K 线切片持有 `BarsListPtr`。
  - `WtCore/WtDtMgr`：tick/K 线切片持有缓存对象；缓存对象被切片共享时追加前写时复制。

## 构建与运行（macOS arm64，Homebrew LLVM 22.1.8，-j2）

```sh
B=/tmp/wt-i03-slice-20261103
# 基线快照：$B/baseline/src（改动前 rsync 的工作区 src，哈希见 evidence/baseline-sha256.txt）
# 静态库（WT_WARNINGS_AS_ERRORS=OFF 为本地临时绕过：WTSDataFactory.cpp 已有他人未用参数告警；
#        dylib 链接因本机 ld 不支持 -no_warn_duplicate_libraries 失败，属环境问题，相关 TU 均编译通过）
cmake -S src -B $B/core -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ \
  "-DCMAKE_CXX_FLAGS=-include cstdlib" -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DWT_WARNINGS_AS_ERRORS=OFF
ninja -C $B/core -j2 WTSUtils WTSTools WtCore WtBtCore WtDtCore WtDataStorage WtDataStorageAD WtDtServo
# 基线同样构建到 $B/core-base
F=src/TestUnits/fixtures/i03_slice_shared_owner_20261103
cmake -S $F -B $B/green-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=... -DWT_LIB_DIR=$B/core/build_x64/Debug/libs \
  -DWT_BASELINE_ROOT=$B/baseline/src -DWT_SAN=address -DWT_GREEN=ON
cmake -S $F -B $B/red-asan ... -DWT_SRC_ROOT=$B/baseline/src -DWT_LIB_DIR=$B/core-base/build_x64/Debug/libs -DWT_SAN=address -DWT_GREEN=OFF
# green-tsan / red-tsan：WT_SAN=thread；bench-new / bench-base：CMAKE_BUILD_TYPE=Release WT_SAN=none
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $B/green-asan/i03_slice_rdm --gtest_output=xml:...
```

ASan 构建排除了 UBSan `alignment` 检查：历史块 12 字节打包头导致记录数组未对齐，属 BUG-R132-001 既有问题，与卡11无关。

## 结果（日志/XML 在 `evidence/`）

| 项 | 基线（红） | 工作区（绿） |
|---|---|---|
| q03 生命周期 5 例 | 4 例断言失败（清理后仍读旧数据），实时 1 例 ASan SEGV | 5/5 |
| 新增 rdm 4 例 | 重复重载断言失败；K 线 heap-use-after-free；并发 bad=2 | 4/4 |
| servo 5 例 | 4 例失败（2 例 abort，含增量更新 heap-use-after-free），部分初始化正控通过 | 5/5 |
| replay 3 例 | clear_cache 两例 heap-use-after-free；跨交易日同尺寸重载被覆盖为新值 + 扩容 UAF | 3/3 |
| unit 6 例 / ABI 2 例 | 不适用（新 API） | 6/6、2/2，LSan 开启无泄漏 |
| TSan rdm 9 例 | 早期 SEGV；并发用例单独跑 0 告警（基线 clearCache 不清历史 map，无并发写，红控弱） | 9/9，0 告警 |
| 相关既有 rdm 测试 177 例 | 175 通过，2 失败 | 176 通过，1 失败（`day_tail_fragment_is_dropped` 两侧均失败，既有）；`ordque_cache_hit_survives_file_removal` 由红转绿 |

微基准（Release、未插桩、与他人构建并行，噪声较大，3 轮交错）：切片创建+释放 基线 79–111 ns，新代码挂 1 个 owner 64–88 ns（同噪声区间）；读取器缓存命中 `readTickSliceByDate` 基线 204–225 ns，新 246–459 ns；`readTransSliceByRange` 基线 475–547 ns，新 507–699 ns。增量来自缓存锁、owner 查找和引用计数，每次 <0.3 µs，判定 B 可接受，不退化为 A。

## 限制

仅本机局部 fake/临时文件验证；静态依赖未插桩；未覆盖 Windows/MSVC、真实外部策略插件、生产数据和整体回归（NOT_RUN）。ABI：仅追加成员，旧头插件可读写新宿主切片（已用 dlopen 旧头插件验证）；但宿主不得对旧插件创建的切片调用 `addOwner`，旧插件也无法使用 owner 接口。持有切片期间仍会让写时复制产生 O(N) 复制。`WtExecMon/WtSimpDataMgr`、`WtDtHelper` 未改。R21-BUG-07 的重复全量扫描未处理。
