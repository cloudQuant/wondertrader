# 费用表候选组件（未接线）

`src/Share/FeeSchedule.hpp` 是独立候选；CTPLoader、实盘引擎和回测均未调用。它只解析费率并返回带状态的查找结果，不计算实际费用，也不表示 I03-NEW-008 已修复。

候选 JSON v2：

```json
{
  "version": 2,
  "contracts": {
    "SHFE.rb2510": {"open": 1.25, "close": 2, "closetoday": 3, "byvolume": true}
  },
  "products": {
    "SHFE.rb": {"open": 0.01, "close": 0.02, "closetoday": 0.03, "byvolume": false, "minfee": 5}
  }
}
```

两个表和四个主要字段均必填，可选 `minfee`、`margin`；数值必须有限，`minfee` 不可为负，费率可为负以表达返点。拒绝重复字段、未知版本/字段、无效键和非法类型。旧无版本 JSON 顶层仅解释为品种表：现有消费者可能将缺项默认为零或 false，但这会把不完整费率误认为合法零费率，因此候选严格要求每个条目提供四个主要字段。数字或完整数值字符串可读取，`byvolume` 要求 JSON 布尔值。该严格子集不支持 YAML、旧版所有宽松类型或 `null`，接入前须明确迁移与兼容策略。无版本文件中形似合约的键不会自动转成合约规则，必须结合基准数据诊断或显式迁移。

查找必须传入已确认的物理合约键和品种键，先查合约再查品种；状态区分 `contract`、`product`、`missing` 和 `invalid`，因此合法的零费率不是缺失。组件只校验键语法及交易所相同，**无法验证合约实际归属该品种或代表成交合约**；接线方必须以实际成交及合约元数据证明这一关系并完成代码规范化，不能直接把连续码或当前日 HOT 映射当作事件身份。无效重载会清空并使表失效，不能继续使用旧费率。

本机在仓库根目录执行（依赖本机 GoogleTest 和 RapidJSON）：

```sh
/usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all -I src -I /opt/homebrew/include -I /usr/local/include src/TestUnits/fixtures/i03_fee_schedule_candidate/fee_schedule_test.cpp /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -pthread -o /tmp/fee-schedule-candidate-test
UBSAN_OPTIONS=halt_on_error=1 /tmp/fee-schedule-candidate-test
```

普通编译及上述局部 UBSan 各 8/8 通过；外部 GoogleTest 未插桩。ASan 的 leak detection 在此平台不支持，关闭后进程在进入 main 前的 dyld/ASan malloc 初始化锁中自旋，故未得到 ASan 测试结果。没有验证发布、实际消费者、并发重载、完整工作树、其他平台或真实柜台。
