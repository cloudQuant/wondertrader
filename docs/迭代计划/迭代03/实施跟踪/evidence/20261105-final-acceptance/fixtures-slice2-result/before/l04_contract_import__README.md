# L04-004：合同导入及边界键的 LMDB 往返

本夹具在 macOS arm64 上直接编译当前 `WTSBaseDataMgr.cpp` 与四个定向测试，使用既有构建中的 WTSTools/WTSUtils 静态库和系统依赖；不是当前整合树的干净构建。运行前需准备好对应静态库：

```sh
bash src/TestUnits/fixtures/l04_contract_import/run.sh /tmp/wt-i03-new084-build
L04_SANITIZE=1 bash src/TestUnits/fixtures/l04_contract_import/run.sh /tmp/wt-i03-new084-build
```

四个测试检查：真实合同配置导入 15 字符交易所和 31 字符合约后，用该合同身份构造 bar/tick 键，分别写入真实 LMDB，关闭并重新打开后读取；原有导入测试验证 16/32 字符身份拒绝、不注册合同和 rules 派生商品，以及混合有效/无效条目的当前部分提交行为；通用对象测试仍允许 31 字符交易所；更新后的导入器测试要求其符合较窄的存储容量，而不改变通用对象容量合同。

普通和局部 ASan/UBSan 最终各 4/4 通过。仅当前产品翻译单元和测试按相应参数编译，已有静态库及依赖未保证插桩；当前平台关闭 leak detection，不声称 LSan 通过。LMDB 往返使用 `WtLMDBQuery` 与当前键类型，并非真实 AD writer、异步完成回执、跨进程并发或真实断电恢复测试。跨平台、整合树和全量回归未运行。

开发过程中首次夹具编译遗漏 `WTSContractInfo` 定义；补充头文件后首个往返测试错误复用已提交的 LMDB 事务，第二次写入失败；改为每次写入使用独立 query 后通过。clang 22 首次构建需在 fmt 兼容头前预包含 `stdlib.h`。脚本初次调整出现 Bash 3 空数组及并发编辑脚本时的解析失败，均已修复并在最终脚本重新运行；这些失败不计通过。
