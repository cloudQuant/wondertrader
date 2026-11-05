# BUG-P05-007 / BUG-R52-002 同工具链定向复验

2026-11-02 本机 macOS arm64。以本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 导出实体快照；运行时工作区的 `WtRdmDtReader.cpp`、`WtDataReader.cpp`、关联测试和 WTSTools/WTSUtils 源与该检查点一致。没有修改产品代码或原共享 CMake。快照另建本目录 `CMakeLists.txt` 的精简目标，仅包含两个 reader TU、两份相关测试、WTSUtils/WTSTools 源以及从本机 GoogleTest 源码编译的测试运行器，不代表原完整构建目标。

全部上述目标的 C/C++ 文件使用 Homebrew LLVM/Clang 22.1.8 和同版本 libc++ 头、`-fsanitize=address,undefined -fno-omit-frame-pointer` 重建，未混入旧静态对象；Boost 头固定 Cellar 1.88.0 实体目录，GoogleTest 源在 Cellar 1.17.0。精确命令见 `build-commands.txt`，目标源码和二进制摘要见 `sha256.txt`。从本机现有动态库加载 Boost filesystem/thread、libiconv、系统 libc++ 与 sanitizer runtime；这些库没有重新构建或插桩，见 `linked-libraries.txt`，不可称全依赖插桩。ASan container-overflow 检查保持默认开启，macOS 不支持 LSan，故 `detect_leaks=0`。

```sh
mkdir -p /tmp/wt-i03-p05-r52-uniform
git archive ccaad010e9169f610aded4d8dc0f5647f70a5cff src | tar -xf - -C /tmp/wt-i03-p05-r52-uniform
cp 'docs/迭代计划/迭代03/实施跟踪/evidence/P05-R52-uniform-20261102/CMakeLists.txt' /tmp/wt-i03-p05-r52-uniform/CMakeLists.txt
/opt/homebrew/bin/cmake -S /tmp/wt-i03-p05-r52-uniform -B /tmp/wt-i03-p05-r52-uniform/build -G Ninja -DCMAKE_C_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
/opt/homebrew/bin/cmake --build /tmp/wt-i03-p05-r52-uniform/build -j 4
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/P05-R52-uniform-20261102/run.py'
```

精简构建的前两次尝试失败：首次缺 gtest/rapidjson 搜索路径及 stdlib 预包含，第二次命令选项去重造成 clang 参数错误；第三次完整编译后链接选择系统 iconv，与现有头声明不一致，改用 `/usr/local/lib/libiconv.dylib` 后构建退出 0。四次输出逐一保留为 `build.log`、`build2.log`、`build3.log`、`build4.log`，不能将早期失败当成功。上述最终 CMake 文件重建时直接采用正确配置。整个快照与构建目录的符号链接扫描结果 `symlinks.txt` 为空；没有创建符号链接。

以每例单独进程与目录、30 秒上限运行，`ASAN_OPTIONS=abort_on_error=1:detect_leaks=0`、`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`；7/7 退出 0，XML 均为 1 测试、0 失败、0 跳过。P05 覆盖单 bar 首前、命中和其后查询及正常数据字段，共 3 项；R52 覆盖空块、单/多条 range、首前/命中/尾后 range 和 count，共 4 项。命令、各自退出码和路径在 `results.json`，输出及 XML 在 `run-*.log`、`result-*.xml`。最小 ASan/UBSan 空进程控制退出 0。此前混链发生的 ASan container-overflow 与链接顺序证据保留在 `../RDM-P05-R52/`，本次通过不能消除当时事实。

本轮只验证列出的快照和定向路径，未验证 cache 缩容/刷新期间借用 slice 的生命周期或并发安全；日线尾碎片的独立合同仍由 NEW-032 跟踪。预编动态库、Windows/Linux、最终整合树与整体回归均未验证；本结果不能代替这两条事项的全部外部验收。
