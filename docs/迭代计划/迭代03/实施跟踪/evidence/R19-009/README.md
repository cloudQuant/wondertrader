# BUG-R19-009 定向复验

使用 R19-007 的隔离源码快照与实体 Boost 优先编译的 CoverageTests_mqshare，相关 ShareBlocks、process/slave 测试文件在运行时与工作区哈希一致。三个相关用例分别在独立进程有界 30 秒内运行，均 exit 0、1/1 PASS 且无 skip：新进程 slave 的提交/删除/刷新、初始 tombstone 不复活、提交后增量刷新。各用例命令为 `CoverageTests_mqshare --gtest_filter=R19TempDir.<文件名> --gtest_output=xml:<路径>`，输出与 XML 在本目录；构建命令/限制见相邻 R19-007 记录。

仅验证本机同 OS 用户、同版本协作路径；新旧二进制混用、多写者、Windows、裸值事务快照和完整当前工作树/整体回归未验证。
