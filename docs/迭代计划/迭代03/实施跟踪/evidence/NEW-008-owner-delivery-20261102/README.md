# NEW-008 owner helper 投递结果竞态（局部修复）

`forEachSinkForOwner` 查表后会释放 `_sink_mutex`，再取得 registration 的锁验证 active/owner/generation。此间 owner 若被注销、sink 被删除或通知被停止，`invokeOwnerSink` 会跳过回调，但旧 helper 仍返回 true。现在内部 helper 将实际调用结果返回给外层：未调用返回 false；执行并正常返回为 true；callback 异常仍向上传播且释放 callback 和 broadcast 计数。此结果不代表下游业务处理成功、事件持久化或账户准入完成。仍保持不持 registry 锁调用外部 callback；注销已进入 callback 的等待/重入行为未改变。没有更改对象字段或虚表，私有非虚函数返回类型改变，相关 TU 应共同重编。

## 测试与复现

本机 macOS arm64，`ccaad010` 加当前工作区；未使用真实 provider/下单、未创建符号链接。当前产品 `src/WtCore/TraderAdapter.{cpp,h}` 和 `diff_owner_sink_registry_test.cpp` 重新编译。隔离构建借用 `/tmp/wt-i03-r42-003-20261102/build` 的静态/动态依赖及其它测试对象，不证明它们与当前完整源码一致。Boost 使用实体 `/opt/homebrew/Cellar/boost/1.88.0/include` 与实体 `cov_boost_include`。`build.py` 与 `run.py` 记录 argv、退出码、源码哈希和逐例日志/XML。测试宏 `WT_OWNER_ROUTING_TEST_HOOKS` 只在定向产品和测试 TU 启用，注入查表后、持有回调租约前的屏障；无测试宏的产品对象另行编译、运行 4/4（原三项 registry 测试 + 异常传播），`nm -u normal-prod.o` 不包含测试钩子符号。测试宏以外的生产构建无此屏障。

基线来自本轮开始前工作树，`baseline/` 是原文件；`red-instrumented.cpp` 仅加测试屏障，未修改返回行为。使用隔离目录将其复制到 `/tmp/wt-i03-owner-delivery-20261102/red/src/WtCore/TraderAdapter.cpp` 并将 `baseline/TraderAdapter.h` 复制到同目录。`build.py red` 用这组源和当前测试构造旧版红控。四个暂停后分别注销、删除、停止、旧 owner 替换新 owner 的用例都 exit 1，断言 `routed` 实际 true 但预期 false；其他原有三个正控及异常控制 exit 0。当前源码同样四个屏障场景与四个正控共 **8/8 PASS**。替换场景还验证旧预约未发给新 owner，新发起的调用可正常送达。重入测试验证 callback 已开始后自行注销时原调用仍返回 true；并发移除测试验证已进入 callback 的移除仍等待排空。每例单独进程、有 20 秒上限。

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-008-owner-delivery-20261102/build.py' red
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-008-owner-delivery-20261102/run.py' red
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-008-owner-delivery-20261102/build.py' green
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-008-owner-delivery-20261102/run.py' green
```

`build-sanitizer.py` 使用 Homebrew LLVM 22.1.8 分别插桩产品和测试 TU，其他链接对象/库未插桩；macOS 系统依赖不受控。ASan/UBSan（关闭 LSan）**8/8**，TSan **8/8**，四个屏障场景普通及 TSan 各重复 10 次共 **80/80**；日志和实际命令在本目录。编译初次分别遇到 LLVM 头文件顺序、rapidjson 搜索路径以及本机旧链接器参数/调试格式警告；失败日志保留。最终 sanitizer 命令预包含 `stdlib.h`、显式 include rapidjson，并移除不兼容的链接器参数及 fatal-warnings，不能视为完整产品工具链验收。

## 未闭合

这只是内部 helper 的局部语义纠正，未接入生产订单/成交/拒单回报；未知 owner 时的事件持久隔离与暂停、账户/owner 账本、注册与 READY 的原子准入、跨重启唯一性及 provider 回报身份均未实现。普通广播仍包含 owner sink，Windows/Linux、真实 SDK、完整工作树与整体回归未验证。NEW-008 继续阻塞。
