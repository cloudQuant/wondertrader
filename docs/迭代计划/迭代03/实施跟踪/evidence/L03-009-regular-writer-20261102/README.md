# L03-009 常规 writer 的 session 日期入口

本项只处理 `WtDataStorage/WtDataWriter` 的 tick 写入，不关闭 L03-009。对有合约 tick，`writeTick` 在 retain/入队前检查 action date 非零、commodity/session 存在且 `tryGetOffsetDate` 可表示；失败返回 false。worker 开始处理前及 `canSessionReceive` 同步回调返回后重新验证，避免已排队的数据或重入回调使日期失效后仍写缓存/文件或广播；`updateCache` 也在锁、扩容和索引发布前检查。同步 `procTick` 通过私有 bool 报告这些日期失败；其余旧过滤分支仍保持 writeTick 原返回语义。旧无合约 tick 仍为已接受但不处理。既有磁盘格式不变。

## 证据

`fresh-final/results.json` 和各日志/XML 是 6 个直接关联用例的独立进程结果。本轮前 `HEAD` 的 writer 源与头加当前 session/time 头构成红控：同步非法日期、异步非法日期、排队后受控变化以及同步 sink 重入变化分别 exit 1；零/非法日期与缺 session 例先断言失败，随后旧代码对空 session 的调用触发 UBSan（exit -6）。最终源码这 5 项及无合约保持 no-op 的正控共 6/6 PASS、exit 0；无合约正控红版同样通过。合法夜盘回退、缓存文件不变、无实时 tick 文件和广播计数均在测试中检查；排队后变更仅在上一任务阻塞于 sink 时由测试线程同步修改，不能证明任意并发修改安全。

`final-controls-results.json` 是同一最终产品对象配既有 P10 测试分别运行的 4 项相关控制（停止时排空已接受任务、同步 lease、关门前 enqueue、回调重入关闭），各 1/1 PASS。产品、测试 TU 用 LLVM 22 ASan/UBSan 插桩，LSan 关闭；WTSTools/WTSUtils 和 Boost 等库复用 `/tmp/wt-i03-r41-drain/libs` 及 Homebrew 预编库，不是整树统一构建。测试未连接真实行情或交易。`sources-final.sha256` 与 `fresh-final-hashes.sha256` 列明关键工作树/快照及产物哈希相同；当前快照之外的全部文件并未冻结。

重建方式（输出目录须不存在；依赖本机预编库）：

```sh
bash docs/迭代计划/迭代03/实施跟踪/evidence/L03-009-regular-writer-20261102/rebuild.sh /tmp/wt-i03-l03-writer-fresh-new
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python docs/迭代计划/迭代03/实施跟踪/evidence/L03-009-regular-writer-20261102/run.py /tmp/wt-i03-l03-writer-fresh-new /tmp/wt-i03-l03-writer-results-new
```

构建脚本先检查源树无符号链接，再实体复制源码；红控仅替换 writer 源和头为当前 HEAD，其他源码及测试与 green 相同。`fresh-final-build.log` 记录命令；未创建符号链接。两个 writer 源/头在本轮开始前没有未提交差异，其他工作区内容没有覆盖。

## 未闭合与兼容边界

- 零 action date 不再隐式使用墙钟；真实 provider 是否允许零日期未验证。新交易日首个 tick 的 session 日期与 trading date 相互关系仍沿用旧行为，不声称全路径交易日合同闭合。
- 异步 `true` 仅表示任务已接收；若排队后数据或 session 变化导致 worker 拒绝，当前只记录日志，没有任务级机器可读失败回执或持久对账。任意并发修改 tick、session/config 及其对象生存期仍未验证。
- 私有 `procTick` 返回类型改变，未更改虚接口或对象字段；相关产品 TU 应一起重编。真实 provider、其他平台、最终整合树与完整回归未验证，不能外推 R23-006 跨进程映射安全或被自动审核中止的 reader slice 工作。
