# BUG-R68-003 同一模块重复加载子项

模块加载获得已存在 handle 时，仅平衡本次新增 loader 引用并跳过 creator，保留旧 factory/worker；不调用签名有冲突的 remover。真实计数插件红控在旧源码产生两个 factory 并失败，新版相关测试和第一方正控 2/2 PASS（red/green XML）。独立 CMake harness 构建 fixture 后，测试默认构建路径和 env 覆盖各 PASS；不存在的 env 路径明确失败，不算跳过。命令 JSON、日志和源码哈希在本目录。

仅处理顺序扫描同一 handle；不同模块同名、并发加载、worker/factory 删除、callback drain、Windows 与第三方 provider 仍未验。整个仓库 CMake 尚未因 fixture 变更完成配置与构建，其余链接依赖复用 v8；BUG-R68-003 保持阻塞。
