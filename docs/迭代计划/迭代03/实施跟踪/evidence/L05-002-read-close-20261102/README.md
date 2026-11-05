# BUG-L05-002：CSV 导出输入文件关闭状态

本子项保留原有 `dump_bars`/`dump_ticks` 的 C ABI、void 返回与 CSV 内容；仅这两个导出入口改为检查数据源读取的打开、完整读取及关闭状态。输入关闭失败时输出读取失败诊断，不创建对应 CSV，不发文件或批次成功日志，继续处理其他输入。原 `readHelperFile` 和其他读取入口不变。原实现使用 `StdFile::read_file_content`，该函数检查短读但不检查输入流 `close()` 的结果。

测试 hook 在真实关闭完成后模拟一次输入关闭失败，并不注入真实操作系统或磁盘故障。`controlled-old.cpp` 在本轮源码基础上将仅 CSV 的读取路径恢复为 `readHelperFile`，注入标志模拟原逻辑无法观察关闭失败；其包含路径被改写为本工作区实体绝对路径，其他本轮代码保持一致。该受控代码不是干净 HEAD 的完整旧版本。两项针对输入关闭的红控分别失败，退出 1（`red.log`、`red.xml`）：仍写入 CSV 且显示成功。当前版本全部两种导出相关测试 `20/20` 通过、无 skip，退出 0（`green.log`、`green.xml`）；包含两项输入关闭失败、输出打开/写入/关闭故障、权限与合法文件正控。`before.sha256`、`after.sha256` 记录本项涉及源码与产物，`baseline/` 保留修改前脏工作树三份文件。

macOS arm64，实体 Boost 头及动态库，未创建符号链接；以下命令从仓库根目录运行，其他 WTSTools/WTSUtils 静态库复用 `/tmp/wt-i03-r41-drain/libs` 既有构建，不代表最终全树重编。编译脚本在本目录，短写用例使用 RLIMIT_FSIZE；文件权限用例在本次非 root 用户下实际执行。

```sh
sh docs/迭代计划/迭代03/实施跟踪/evidence/L05-002-read-close-20261102/build.sh /tmp/wt-i03-l05-readclose-20261102/controlled-old.cpp /tmp/wt-i03-l05-readclose-20261102/red
sh docs/迭代计划/迭代03/实施跟踪/evidence/L05-002-read-close-20261102/build.sh src/WtDtHelper/WtDtHelper.cpp /tmp/wt-i03-l05-readclose-20261102/green
LLVM_PROFILE_FILE=/tmp/wt-i03-l05-readclose-20261102/green-%p.profraw perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-l05-readclose-20261102/green '--gtest_filter=L05HelperBarTest.DumpBars*:L05HelperTickTest.DumpTicks*'
```

使用 `build-sanitizer.sh` 的 Homebrew LLVM 22 局部 ASan/UBSan 构建、`detect_leaks=0` 同筛选 20/20 通过，见 `asan.log`、`asan.xml`；产物源与两个测试 TU 插桩，预构建依赖和系统库未插桩。首次编译缺 `/usr/local/include/rapidjson` 路径失败并记录于 `asan-build.log`；增加实体头目录后重编通过，记录 `asan-rebuild.log`。未定义测试 hook 的产品源语法编译通过（`no-hooks-syntax.log`）。

边界：旧 void API 且 logger 为 null 时仍无法告知调用方失败，仓内未发现可迁移的产品调用方；对部分输出文件不承诺事务发布或恢复，真实 OS 关闭失败/真实存储故障、断电持久性、并发目录变化、Windows 与完整树均未验证。数据块魔数/版本和非目标 tick 类型语义另待确认，不由本项测试关闭。整项仍不能据此验收。
