# K45：CTA 策略快照只兼容当前形状，未知形状整体拒绝（BUG-P01-004）

- 产品：`src/WtCore/CtaStraBaseCtx.cpp` `validateCtaSnapshot` 在原有类型/必填/边界预检之外，对根、fund、positions[]、details[]、conditions、条件项、signals、utils 各层校验成员名：写端（`save_data`）从不产生的成员视为未知形状，记字段路径并整份拒绝（暂存候选不发布、不订阅）。已知可缺省字段（frozen/frozendate、opentdate、maxprice/minprice、openbarno、fund、signals、utils、条件项 code）兼容不变。行尾保持 CRLF。
- OOM：`load_data`/`on_init` 无吞异常路径，恢复期 `std::bad_alloc` 按 K45 作为 context 致命错误向上传播（静态核对，未做注入测试）。
- 用例：`R42CtaPersistTest.K45UnknownMembersRejectTheWholeSnapshot`（8 层各插入一个未知成员：拒绝、错误含路径与成员名、基线状态与订阅不变；去掉未知成员后同一候选可恢复）。
- 红控：改前源文件重编 `CoverageTests_wtcore`，新用例失败（`red.log`）；恢复改后（哈希一致，`sources.sha256`）后 R42CtaPersistTest 16/16（`green.log`）；wtcore/porter/m_WtRunner 逐例 1349 通过（`regression-summary.json`）。
- 未覆盖：恢复期 bad_alloc 注入正反控；早期 ASan container-overflow 报告的同版本 libc++ 复核（X）；外部订阅失败后的部分订阅与写端截断原子性。
