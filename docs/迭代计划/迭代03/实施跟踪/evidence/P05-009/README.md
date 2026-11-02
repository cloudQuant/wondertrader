# BUG-P05-009 私有 LMDB 读取 helper 定向结果

新增 bool + 输出参数重载，以闭区间遍历并逐条复制有效 K 线；查询、键和值错误时返回 false 并保持原输出。旧 string 签名保留为兼容包装，仍无法区分空结果和失败。仓库中没有该 helper 的生产调用，不能由本结果声明生产读取链路已验证。

隔离 LMDB 定向 4/4 PASS（`focused-final.xml`）：空/单/多条、闭区间端点及其他 code，短长 value 与异常 key、查询过程中注入失败和输出不变、无效身份及周期。旧源码非零键丢失红控 1/1 失败（`red.xml`）；红控源码及编译命令保留在本目录。当前 reader 和测试各重编一个 TU，链接其余对象与库复用 v8；编译命令为 `commands.json`，对应哈希见 `sha256.txt`。本机 macOS arm64；未运行 ASan、Windows/Linux、当前树完整构建及整体回归。旧包装的错误语义与后续使用合同仍需明确。
