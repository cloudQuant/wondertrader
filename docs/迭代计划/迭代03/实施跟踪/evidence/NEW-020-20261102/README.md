# BUG-I02-NEW-020：撤单引用与 UDP 对齐局部定向验证

2026-11-02，macOS arm64。检查点 `ccaad010` 的实体 `src` 快照覆盖当前 `TraderMocker.cpp`、`WTSCfgLoader.cpp/.h`、`r113_mocker_common.h` 及新增测试；其余源取检查点。产品源包含 R60-003 排队引用 RAII 工作区修改。源码和二进制哈希见 `sha256.txt`；快照/构建目录 `find -type l` 结果见空文件 `symlinks.txt`，未创建符号链接。该隔离快照不是最终完整源码树。

## 构建

先执行：

```sh
mkdir -p /tmp/wt-i03-new020-20261102
git archive ccaad010 src | tar -x -C /tmp/wt-i03-new020-20261102
# 分别复制当时工作区版本的上述五个覆盖源到快照对应位置。
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -fsanitize=address,undefined /tmp/wt-i03-new020-20261102/asan-control.cpp -o /tmp/wt-i03-new020-20261102/asan-control
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 perl -e 'alarm 20; exec @ARGV' -- /tmp/wt-i03-new020-20261102/asan-control
/opt/homebrew/bin/cmake -S /tmp/wt-i03-new020-20261102 -B /tmp/wt-i03-new020-20261102/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/Cellar/ninja/1.12.1/bin/ninja -DCMAKE_C_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
/opt/homebrew/bin/cmake --build /tmp/wt-i03-new020-20261102/build -j 4
```

空控制源码 `int main() {return 0;}`，编译和运行各 exit 0。最终隔离 CMakeLists 存本目录：与产品目标不同，仅重编目标测试、TraderMocker、WTSTools、WTSUtils 和 GoogleTest 源，全部使用 LLVM 22 ASan/UBSan；系统 Boost 动态库、libiconv、libc++ 未插桩，macOS 不支持 LSan。第一次 configure 未指定 Ninja 路径，exit 1，见 `configure.log`；重新配置与最终构建 exit 0，见 `configure2.log` 和 `build3.log`。

## 失败与修复

原夹具先在 `#pragma pack(1)` 对象中构造 WTSTickStruct，两个定向测试在夹具本身的未对齐构造均 UBSan exit 134（`result1.log`、`result2.log`），未执行撤单断言。夹具改为对齐局部对象序列化为原线协议后，旧产品 UDPDataPacket 的构造仍未对齐，两项各 UBSan exit 134（`final1.log`、`final2.log`）。

产品从原有四字节包头后的数据复制到对齐局部 tick，再使用该对象；短包检查和线协议保持。新增 `NEW020UdpAlignment.CompleteTickAcceptedAndTruncatedOrWrongTypeIgnored` 验证完整 tick、短头/短 payload、错误类型与后续完整 tick。两个原验收测试不改断言，保留调用者提前释放引用的屏障和订单移除检查。

## 最终定向运行

每项在独立目录，以 50 秒上限运行；以下 `TEST`、`RUN` 分别对应表中值：

```sh
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 perl -e 'alarm 50; exec @ARGV' -- /tmp/wt-i03-new020-20261102/build/new020 --gtest_filter=TEST --gtest_output=xml:/tmp/wt-i03-new020-20261102/RUN.xml
```

| RUN | TEST | 结果 |
|---|---|---|
| run1 | `R113MockerPartialCancel.R113Mocker_CancelCopiesOrderIdBeforeFinalActionRelease` | 1/1 PASS，exit 0 |
| run2 | `R113MockerPartialCancel.R113Mocker_PartialFillThenCancelPushesCanceled` | 1/1 PASS，exit 0 |
| run3 | `NEW020UdpAlignment.CompleteTickAcceptedAndTruncatedOrWrongTypeIgnored` | 1/1 PASS，exit 0 |

日志及 XML 同名保存在本目录。`git -c core.whitespace=cr-at-eol diff --check` 对本次修改的已跟踪源码/夹具退出 0。

证据只覆盖单机模拟器、所列调用路径；ReleasePoison 是逻辑释放哨兵，不证明真实对象池同址复用。没有验证真实 SDK、其他平台、并发关闭、自销毁、全部依赖插桩或最终整合树；内存工具的通过不等于总体回归通过。
