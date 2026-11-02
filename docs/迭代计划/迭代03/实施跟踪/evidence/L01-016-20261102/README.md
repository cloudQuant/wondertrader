# BUG-L01-016 月码定向探针（2026-11-02）

这是当前源码的单翻译单元探针，不是修复后验收。没有修改产品代码，亦未调用真实交易接口。探针源码见 [probe.cpp](probe.cpp)，普通运行输出见 [normal.log](normal.log)。

执行时 `src/Share/CodeHelper.hpp` 的 SHA-256：`e0df6ae4f630a445e73df36fc939e4dc606a9263b2b86d83609e4f9d32c4f65d`；探针源码 SHA-256：`01dbe493e5e839f169caae065bf2aab84c5548cce0426766d3d69f6a47eecf8f`；普通二进制 SHA-256：`43a9a1d5563ad77fb1c2293bdd21c85574a6949c43fe789d2002c4cb89ba675f`。二进制和复制的依赖未归档。

## 构建与运行

在仓库根目录执行。先使用以下命令将依赖解引用复制到隔离临时目录，未创建符号链接：

```sh
mkdir -p /tmp/wt-i03-l01-016-probe/include && cp -RL /opt/homebrew/include/boost /tmp/wt-i03-l01-016-probe/include/boost && cp -RL /opt/homebrew/include/fmt /tmp/wt-i03-l01-016-probe/include/fmt
```

使用本证据目录的 `probe.cpp` 作为 `/tmp/wt-i03-l01-016-probe/probe.cpp`，普通构建与运行命令：

```sh
clang++ -std=c++17 -DSPDLOG_FMT_EXTERNAL -I/tmp/wt-i03-l01-016-probe/include -I. /tmp/wt-i03-l01-016-probe/probe.cpp -o /tmp/wt-i03-l01-016-probe/probe-normal && /tmp/wt-i03-l01-016-probe/probe-normal
```

退出码 0。之后单独运行同一二进制并将标准输出保存为 `normal.log`，退出码 0。合法 3/4 位样例产生原有结果；`ag12-4` 生成非数字伪码；`isMonthlyCode` 对 `BTC202412` 和 `BTC202499` 均返回真，而转换结果均为 `SHFE.BTC.2202`。该两个输入证明当前算法碰撞，不能据此推定哪一种新编码合同正确。

## Sanitizer 尝试

```sh
clang++ -std=c++17 -DSPDLOG_FMT_EXTERNAL -I/tmp/wt-i03-l01-016-probe/include -I. -fsanitize=address,undefined -fno-omit-frame-pointer /tmp/wt-i03-l01-016-probe/probe.cpp -o /tmp/wt-i03-l01-016-probe/probe
ASAN_OPTIONS=detect_leaks=1 /tmp/wt-i03-l01-016-probe/probe
ASAN_OPTIONS=detect_leaks=0 /tmp/wt-i03-l01-016-probe/probe
```

插桩构建退出码 0。首次运行退出码 134，报告 `AddressSanitizer: detect_leaks is not supported on this platform.`；第二次无输出后由操作者中断，退出码 130。两次均不是 sanitizer 通过证据，也不能证明存在或不存在内存缺陷。

## 调用链和剩余工作

`WtCtaEngine.cpp` 的多个调用处和 `WtSelEngine.cpp` 会将转换值用于目标或仓位；`HftStraBaseCtx.cpp` 的多个交易路径使用转换值建立映射或发单。相比之下，`ParserAdapter.cpp` 和 `WtDtRunner.cpp` 已对空结果进行局部拒绝。上述判断是静态分析；没有进行交易链动态试验。

月码五、六位的标准身份合同及其与现有四位标准码的兼容性仍需确定；只有在所有受影响调用链能安全处理转换失败后，才能将非法输入转换为空结果。BUG-L01-014 的账户级隔离及恢复、其他平台和整体回归均不在此探针证明范围内，本项仍未完成。
