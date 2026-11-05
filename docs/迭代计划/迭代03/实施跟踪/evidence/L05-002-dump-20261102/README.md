# BUG-L05-002：CSV 导出读写失败传播子项

本轮对 `dump_bars` / `dump_ticks` 使用检查写入和关闭结果的 CSV 写入路径：打开、写入或关闭失败记录目标路径并继续处理其他文件，取消失败文件的成功日志及本批“全部导出完成”日志；源文件读失败、短头或数据块损坏同样取消本批成功日志。`dump_bars` 对不是 K 线类型的已识别数据仍按原规则跳过。原 `void` 导出接口及成功 CSV 格式保持不变；输出失败时已产生的部分文件不会自动删除。

`StdFile::write_file_content` 原接口是 void：打开失败会抛出 `std::runtime_error`，其 write/close 不检查流状态，不是忽略了 bool 返回值。新路径保留原 `std::ofstream` 的写打开方式，检查 write 和 close 状态；缓冲短写可能在关闭时报告，关闭检查不含 fsync 或断电持久性保证。Windows 路径字符编码、真实短写/关闭故障和 MSVC 本机均未验证。

## 构建与运行

仓库根目录，macOS arm64 Apple Clang，实体 Boost 1.88 头；没有创建符号链接。`build.sh` 是本轮实际编译命令，复用 `/tmp/wt-i03-r41-drain/libs` 中先前编译的 WTSTools/WTSUtils，因此不是全依赖一致的干净构建。GTest、动态库和其他依赖也非当前源码整树验证。首次不带覆盖率 runtime 的链接因旧库需要 `___llvm_profile_runtime` 失败，添加 `-fprofile-instr-generate -fcoverage-mapping` 后构建成功。源与产物哈希见 `sha256.txt`。

旧版产品源码从 `git show HEAD:src/WtDtHelper/WtDtHelper.cpp` 提取，使用当时新增的四项定向测试构建并运行，过滤两个输出路径阻塞及两个 RLIMIT_FSIZE 短写用例：4/4 失败，进程退出 1，完整输出和 XML 为 `baseline.log`/`baseline.xml`。旧代码在文件打开失败抛出异常，而短写时错误输出文件和批次成功日志。测试源此后增补了其他测试，故 `sha256.txt` 中是最终测试文件而非该次反例时完整测试 TU 的哈希；四项反例定义没有改动。

```sh
sh docs/迭代计划/迭代03/实施跟踪/evidence/L05-002-dump-20261102/build.sh \
  src/WtDtHelper/WtDtHelper.cpp /tmp/wt-i03-l05-dump-20261102/final
LLVM_PROFILE_FILE=/tmp/wt-i03-l05-dump-20261102/final-%p.profraw \
  perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-l05-dump-20261102/final \
  '--gtest_filter=L05HelperBarTest.DumpBars*:L05HelperTickTest.DumpTicks*' \
  --gtest_output=xml:/tmp/wt-i03-l05-dump-20261102/final.xml
```

首轮 BoostFile 方案 14/14 定向通过（`final.log` / `final.xml`），但独立审查发现其以读写模式打开，可能拒绝原本可写但不可读的文件，不能作为最终代码验收。覆盖可读回的正常 CSV、K 线导出跳过合法非目标类型、短头/非法数据、输入权限拒绝、父目录非目录、输出文件目标被同名目录占用、多文件部分失败后其他文件仍导出、RLIMIT_FSIZE 短写，以及 K 线关闭后的注入失败。关闭注入在真实 close 完成后修改结果，非真实关闭故障。无测试宏产品源单独 `-fsyntax-only` 编译退出 0。

这是导出子项的本机证据，不证明实际磁盘故障、部分文件清理/事务原子性、Windows/其他平台、底层所有错误或电源故障；未知/非目标 tick 类型合同、返回 void 无法向没有 logger 的调用方报告失败等仍需单独评估。整体回归未运行。

## 写权限兼容与目录迭代复核

`write_only_probe.cpp` 在 macOS 非 root 进程对 owner-write-only CSV 测得 BoostFile read_write 打开失败、原 ofstream 模式写入和关闭成功，探针编译、运行退出 0，输出见 `write-only.log`。因此 CSV 单独使用 ofstream 并检查 write 和 close，而未更改 BoostFile。两条新的写权限测试验证同样场景的 bar、tick 内容和成功日志。源目录迭代器创建、状态读取和递增使用错误码；不可遍历目录不再抛出且无批次成功日志，源路径为普通文件也验证不会输出成功。

ofstream 会缓冲：在第一次补充验证中，RLIMIT_FSIZE 触发的短写在 close 期间体现为“关闭失败”，原先仅接受“写入失败”的两个测试因而失败；退出 1，见 `final2.log` / `final2.xml`。现断言接受写入或关闭失败，但仍严格禁止文件及批次成功日志。最终使用同一 `build.sh` 重新构建，运行筛选 `L05HelperBarTest.DumpBars*:L05HelperTickTest.DumpTicks*`，18/18 PASS、无 skip，退出 0；见 `final3.log`、`final3.xml` 和 `final3-sha256.txt`。测试与库仍是局部重编，且不证明迭代期间目录被修改的所有竞态。
