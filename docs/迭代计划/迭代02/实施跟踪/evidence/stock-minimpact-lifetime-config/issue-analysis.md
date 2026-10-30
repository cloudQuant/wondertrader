# StockMinImpact 独立问题记录

这两项来自同一执行单元，但根因和修复条件不同，分别登记为 BUG-I02-NEW-005 和 BUG-I02-NEW-006。

## BUG-I02-NEW-005：释放借用的商品信息

WtLocalExecuter::getCommodityInfo() 透传基础数据引擎持有的商品信息指针，没有转移所有权。StockMinImpactExeUnit::init() 会对自己保存的 _comm_info retain，并在析构时释放该自有引用；get_minOrderQty() 只是临时查询商品类型，却曾对借用指针调用 release()。这会提前消耗基础资料所有者的引用，随后执行单元析构或夹具清理可能再次释放已销毁对象。最初的本地对象清理崩溃已由独立调试追到此错误引用计数路径。

当前代码将该查询作为只读借用，并在访问 product 前检查商品和 product 指针；商品资料不足时禁用执行。回归 minimum_order_lookup_keeps_borrowed_commodity_alive、minimum_order_lookup_without_commodity_fails_closed 检查多次读取、保留引用计数和缺失资料行为。对应代码位置见清单。

## BUG-I02-NEW-006：无效 min_order 参与取整和除法

_min_order 曾以 0 初始化；配置缺少 min_order 或给出非正值时，执行单元可能继续启用。do_calc() 将其传入 round_hands()，并直接用于可买量除法，因此可能发生除零或形成非法订单。

当前代码要求配置值存在、有限且严格大于零，否则初始化失败并保持执行禁用。回归 missing_zero_and_negative_min_order_disable_execution 覆盖缺失、零和负值；普通股、科创板和可转债正值路径继续由原回归覆盖。

## 当前证据与未验范围

本目录的 run.log 记录修改后的 34/34 本机定向通过，build.log 记录 WtExeFact 重建，run.json 保存运行范围，reproduce.sh 提供直接复现命令。清单两项暂记 TEST_PASS，表示局部测试完成，不代表完整验收。

尚待验收：首次 Local 定向运行曾有一次无法归因的 SIGSEGV，后来同一二进制重复运行 13/13 并连续跑 20 次均通过；需要保留该失败并继续关注。min_order 的 NaN/Inf 解析行为、旧配置缺少此键时的迁移/兼容约定、跨平台编译、完整 runner/策略/交易 API 链路及真实柜台均未证明。
