# BUG-P08-006 定向验证

使用 R58-003 隔离源码快照及构建：`CoverageTests_btcore` 定向运行 `P08UftOrderPosition.ExitUnknownCommodityRejectedWithoutStateMutation` 和 `ExitLongAndShortWithKnownCommodity`，各 1/1 PASS。产品 UftMocker.cpp、测试文件在运行时与工作区哈希相同，见 sha256.txt；测试目标相关 TU 优先使用实体 Boost bridge。

其他依赖来自隔离快照，快照与工作区部分其他文件不一致；依赖中有系统 Boost 链接路径。仅验证本机双向平仓的未知/已知商品场景，其他平台、完整工作树及整体回归未验证。
