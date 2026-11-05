# T2-CTP-08：CTP 单实例身份基础

当前实现只覆盖 CTP 普通报单路径。`makeEntrustID` 与缺失 ID 的 `orderInsert` 使用同一单实例预留记录；提交已有本实例、当前登录代次的合法预留时保留该编号，其他或重复身份拒绝。缺 tag 使用 `ctp-auto:` 加规范 ID 并写回；其他 tag 保持原样。关联写入在 SDK 发送前进行，已有缓存键（包括空值）不覆盖；SDK 返回非零或抛异常时已使用的编号不回收。缓存写入是关联记录，不证明断电持久性。

身份及本地缓存分别由互斥保护；不持身份锁跨 SDK 或宿主回调。登录高水位取本实例现有值与 SDK 报告值的较大者，登录、认证和结算请求按本实例请求/代次限定；发送在途时收到登录回调则关闭身份准入且要求重新登录，不等待自身。报单、成交及拒单关联还限定已登记身份及可用账户、交易日、合约信息；不能验证时不归属本地 tag。`WtCore::TraderAdapter::onRspEntrust` 在触及未完成量前验证自身 `otp.<adapter>.` 前缀与完整非零 uint32 ID，外部或格式不合法的标签不再消耗本地未完成量。

复跑（本机 macOS，目录须不存在）：

```sh
bash src/TestUnits/fixtures/i03_ctp_order_identity_20261103/run.sh /tmp/wt-i03-ctp-identity-review-new
```

脚本单独 `-j2` 构建 TraderCTP、WtCore 及依赖，针对当前源码编译真实 TraderCTP 与 fake SDK 的定向 fixture，并执行 `T25TraderCTPTest.I03Identity*` 加六个既有兼容控制；另执行真实 TraderAdapter 的 `P11PushNotify.entrust_error_updates_undone_once`。精确过滤器见 `run.sh`。项目 WTSDataFactory 的现有未使用参数告警以 `-Wno-error=unused-parameter` 单项降级；CTP fixture 对自身目标仍使用 `-Werror`。构建前后关键源码哈希不一致时整个脚本失败；关联产物与测试日志位于 `evidence/`，原始文件及用户既有脏修改快照位于 `baseline/`。

本机复核过程中，v6 在非登录 shell 下因测试中的有符号比较告警构建失败；只修正测试期望值类型后，v7 因 CMake 错选 Anaconda GTest 库而链接失败。对应失败日志和退出码保留在 `evidence/`；脚本现显式选择 Homebrew GTest 配置，避免头文件和库版本混用。

最终运行目录 `/tmp/wt-i03-ctp-identity-final-v8-20261103`：脚本退出码 0，CTP 过滤集合 21/21、宿主测试 1/1；构建前后哈希一致，输出中未发现符号链接。`evidence/exit.txt`、`ctp-tests.log`、`host-tests.log`、构建日志和 `before.sha256`/`after.sha256` 保存了对应结果；`*.patch` 为相对本轮前快照的增量差异。v5 外围记录退出码时误用 zsh 只读变量，未作为退出码证据。

本结果不证明跨实例/跨进程/跨重启唯一，也不证明缓存断电持久、真实 CTP 柜台回报顺序、无 OrderRef 的先成交后委托关联、并发相同账号其他实例、插件加载/卸载或跨平台。`release`、对象销毁、模块卸载与发单/回调并发需要调用方在外部完成排空与同步，原有 void 接口没有可观察的 deferred 释放协议。其他尚未迁移的旧测试可能仍依赖空 tag 共用 OrderRef 或仅注入旧缓存，不能将本次定向结果当成全量通过。HFT/UFT、CTPOpt、CTPMini 和账户级协调不在本次范围。
