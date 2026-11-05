# BUG-T25-007 标签缓存本机定向复验

2026-11-02，macOS arm64，uid 501。以本地检查点 `ccaad010` 的 `src` 导出到 `/tmp/wt-i03-t25-007-20261102/srcsnap`，使用实体 Boost 1.88、fmt、spdlog 和 GoogleTest 目录分别重编 `TraderCTP.cpp` 与 T25 测试 TU。运行时工作区上述源码及 fake API 头的 SHA-256 与快照一致；哈希、二进制及链接依赖哈希见 `sha256.txt`。未修改产品或测试源码。

```sh
mkdir -p /tmp/wt-i03-t25-007-20261102/srcsnap /tmp/wt-i03-t25-007-20261102/obj /tmp/wt-i03-t25-007-20261102/run
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-t25-007-20261102/srcsnap
bash 'docs/迭代计划/迭代03/实施跟踪/evidence/T25-007-20261102/build.sh'
bash 'docs/迭代计划/迭代03/实施跟踪/evidence/T25-007-20261102/run.sh'
```

构建脚本和完整编译日志已保存。首次构建 `build.log` 失败：源码 include 路径作为普通 `-I`，在 `-Werror` 下厂商 SDK 的未使用参数报错；将源码根路径改成与项目构建一致的 `-isystem` 后，`build2.log` 构建退出 0。此为局部构建，复用 `/tmp/ctpm-i03-build` 的 WTSTools/WTSUtils 静态库，不能证明全部依赖与本次快照一致；无 sanitizer 运行结论。测试范围仅三个指定用例，不是完整测试目标。

三个用例在各自进程和临时目录中运行，每个以 40 秒 alarm 限时，均退出 0：只读缓存目录登录时记录告警并维持登录状态；缓存未初始化时 tagged 下单返回失败且 fake API 无发单；可写缓存中的 tag 正常传递到订单及成交。XML 均为 1 test、0 failure、0 skipped，见各自日志及 `results.txt`。

测试使用与测试临时目录相同文件系统的独立 0500 目录，以当前 uid 实际尝试创建文件并得到 `Permission denied`，`permission.log` 记录了该权限正反控；不是仅凭 root 下 chmod 推断只读。快照、构建及运行目录扫描无符号链接（`symlinks.log` 为空），本轮未创建符号链接。

测试使用受控 fake CTP API，不连接真实柜台；不能证明真实供应商回报、其他平台、真实磁盘故障、全依赖插桩或最终整合树及整体回归。当前结果仅表示上述本机局部行为通过。
