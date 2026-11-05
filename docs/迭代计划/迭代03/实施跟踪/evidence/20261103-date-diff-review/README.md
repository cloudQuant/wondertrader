# 日期映射与目标批次第二轮复核

本记录延续 2026-11-03 的修复，不改变冻结的 213 项分母。全量回归仍为 `NOT_RUN`，新增问题仍独立跟踪。

## 日期映射 V1

主线程对照 fixture 中的修改前快照审阅了 WTSBaseDataMgr 的增量。新增非虚 checked 方法及显式版本函数表，没有向 IBaseDataMgr 添加虚函数，也没有向 manager 添加实例字段。日期、时间、缺商品、缺时段、缺日历等情况通过 status 区分；失败 value 为 0，但调用方必须判断 status。新接口不会自动绑定未知的 IBaseDataMgr 实现。

checked 路径使用单次 checked 本地时间快照和有范围上限的公历逐日移动；它不依赖一天固定 86400 秒的本地时间运算。工作日恰为配置节假日时保留既有 calc/getTradingDate 行为，不能据此声称重新定义了日历业务。旧入口只增加必要输入保护，仍保留其 sentinel/fallback 限制。

主线程独立使用当前 WTSBaseDataMgr.cpp，以 `-Wall -Wextra -Werror` 和 `-j2` 在 `/tmp/i03-date-mapping-root-20261103` 构建，日期 fixture **5/5 通过**，无跳过。覆盖非法日期/时间、闰日、范围边界、正负 offset、7x24、周末和 holiday 跳转、缺信息状态、缓存失败不发布以及版本表校验。所链接 WTSTools/WTSUtils 依赖复用 `/tmp/wt-i03-r58-20261102-build`，并非完整依赖闭包重编。worker 另报告关联 Session 48/48，主线程本轮未重新运行这 48 项。

构建后公开头只补充了 API 合同注释；归档哈希表示归档时的当前文件，不冒充原子构建证明。时钟读取失败、内部异常、并发配置/缓存、外部插件、Windows 和完整回归未验证。接口仍要求 manager 存活及调用者遵守既有外部同步约束。生产消费者迁移另行记录，不能以接口 fixture 关闭整个日期问题。

## HFT 回放范围消费者

`HisDataReplayer::get_hft_trading_date_range` 迁移到 checkedCalcTradingDate。任一订阅映射失败都会返回 false 并记录 code/status，不再用自然日代替失败结果。范围在临时变量中累积，全部成功后才发布输出；无订阅、非法输入和缺元数据失败均保留调用方原输出。显式回放边界的日期 0 被拒绝，不能解释为当前日期。

主线程对照本轮修改前快照审阅了该函数，并在 `/tmp/i03-replayer-date-range-root-20261103` 独立重编当前 Replayer、BaseDataMgr 和测试，局部 ASan/UBSan **20/20 通过**，无跳过；LSan 关闭。其余静态依赖复用 `/tmp/ctpm-i03-build/build_x64/Debug/libs`，不能外推为整工程 sanitizer 验收。正常日盘、夜盘、7x24、四类订阅、混合缺信息和输出保留均有控制。

现在即使日盘/7x24 也要求加载对应日历，这是从 silent fallback 到显式失败的兼容性变化。其他 Replayer 日期调用仍未全部迁移。调用方的 bool 仍不能区分无订阅与映射错误：纯 tick 路径在 false 后结束，bar+HFT 路径仍可能继续 bar 流程；外部完成通知也未新增错误状态。因此本项只证明范围函数的拒绝和输出发布，不证明端到端失败回执或完整回放已闭合。

## 执行器目标批次

主线程审阅本轮任务前快照与当前 WtDiffExecuter 的增量，并要求修复了同值目标缺 unit 时不恢复、未就绪 omitted 清零少通知、空 code 后置拒绝三个边界。目标计算在临时映射内完成，数值检查全部通过后，在同一个 update 临界区提交账本、持久化和排队；注册前更新由注册时快照捕获，注册后更新按 FIFO 排在恢复后，避免读取最新 diff 越过已排队的 trade。外部 factory、initialize 和 callback 不在该更新锁内运行。

提交前到达的增量仍拥有对应目标；较新的有效 batch 可以清理较早在途 batch 新引入的遗漏目标。两项旧测试依赖逐项提交的中间值，按整批先提交的行为改为 20 后增量到 22，以及已提交的遗漏目标 5→0→7；原有 trade 不丢失断言保留。持久化失败仍保留 dirty-in-memory 账本，所有本批变化键一起禁止自动派发；这不是磁盘与内存回滚事务，也不是跨合约订单原子成交。

worker 实体快照初编后同步最终文件并增量重编：独立过滤集 17/17，合并小数及 Arbi 控制 28/28。主线程核对关键产品/测试源码与快照一致，另在 `/tmp/i03-diff-batch-root-20261103` 独立严格告警编译 WtDiffExecuter.cpp，替换该 object 重新链接（其余 objects/库复用 worker 的冻结构建），独立运行 **17/17 通过**，无跳过。命令、输入核对、日志、XML 和本轮差异已归档；未声称主线程重编了完整依赖闭包。

新增测试位于专项 fixture，经 prepare 脚本加入隔离目标。注册前重入和同值恢复通过 friend 构造状态并调用真实 registerCreatedUnit；未验证动态 factory 完整 initialize 链。存储失败使用注入，真实设备故障、OOM、完整生命周期、真实 provider、跨平台及全量回归未验。Local/Arbi 多组净额、负/小数组合匹配和组/账户双重限额仍未完成，因此 P02-005 保持阻塞。
