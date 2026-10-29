# BUG-L00-005 当前链接目标边界

- 产品 runner 的核心库分离：`src/WtRunner/CMakeLists.txt:21-25` 只列 `WtCore`，`src/WtUftRunner/CMakeLists.txt:21-25` 只列 `WtUftCore`。这不支持“当前这两个产品 runner 把两份 `ActionPolicyMgr` 同时放进同一链接单元”的说法。
- 覆盖率 CMake 将核心家族拆开：`src/TestUnits/coverage/CMakeLists.txt:141-151` 分别指定 WtCore/UftCore/BtCore/DtCore 测试目标；` :253-278` 为每族创建独立目标并只链接共享测试依赖。N01 和 N03 测试分别写明覆盖 `src/WtCore/ActionPolicyMgr.cpp` 和 `src/WtUftCore/ActionPolicyMgr.cpp`。
- 生成的 `build-cov/build.ninja` 表明主 `CoverageTests` 仍在 `:12977` 链接 `WtDtCore` 与 `WtCore`，并通过依赖带入 `WtBtCore`；`CoverageTests_m_WtUftRunner` 的生成链接行也包含多个核心归档。静态归档中的重复符号本身不能证明特定成员已抽取。
- `targeted-runs-second-round.json` 中，`N01ActionPolicy.init_parses_default_group_and_maps_every_action` 与 `n03_action_policy.init_loads_groups_and_filters` 各自 1/1 PASS。`current-target-core-source-map.json` 将 N01 的 ActionPolicy 符号映射到 `WtCore/ActionPolicyMgr.cpp`，将 N03 映射到 `WtUftCore/ActionPolicyMgr.cpp`。
- `CoverageTests_m_WtUftRunner` 的既有 help 探针提前退出，未生成 GTest XML；不得计为 GTest PASS，也没有覆盖 ActionPolicy 路径。
- 尚未完成：对混合核心归档的测试目标选择安全的现有用例，保存真实链接成员/符号绑定及覆盖源文件映射；确认是否有错抽取后，再决定测试目标是否需继续拆分。这个缺口使 37 个跨目录同名类的整体结论保持未决。
