# BUG-L03-002：商品构造字段与配置边界

2026-11-02，macOS arm64。此目录是诊断探针，**不是修复或商品对象可投入交易的验收**。未修改产品头文件，也未运行真实行情或交易。

`WTSCommodityInfo` 构造函数目前只初始化身份数组、session 指针和手续费标记；`m_uVolScale`、`m_dPriceTick`、`m_dLotTick`、`m_dMinLots` 及四个交易相关枚举没有初始化。`create` 不为它们调用 setter。`WTSBaseDataMgr::parseCommodity` 在正常加载路径为这八项赋值，但它不是 `create` 的一部分；直接创建或部分配置的对象可被 getter 读取。`SelMocker` 和 `CtaMocker` 有按 price tick 作除数的路径，不能凭零初始化推断安全。

仓库枚举零值分别具有具体含义：`CC_Stock`、`CM_OpenCover`、`PM_Both`、`TM_Both`，尤其 `TM_Both` 表示可做多空；它们不是统一的未配置标记。配置解析使用 `CC_Future`、`TM_Both`、手数精度与最小手数 `1` 的路径级默认，也不能据此规定所有直接工厂使用者的默认合同。枚举虽有 `PM_None`/`TM_None`，但类别缺少已定义的未知值，而已有调用方是否会在交易前检查状态亦未证明。

探针以 `0xa5` 预填同一对齐存储，再 placement-new 一个仅公开受保护构造函数的派生对象；不调用未初始化字段的 getter。通过 `unsigned char` 复制和比较对象表示，观察每个 setter 写入的字节在构造后仍与预填值相同，并在全部显式设置后调用 getter 做正控。两个构建均观察到 8 个字段对应字节保留预填值、完整显式配置后正控通过。此为当前编译器、架构和探针的对象表示观察，**不是未初始化读取的有效值，也不是未定义读取的安全许可**。在公开 `create` 之后另外完整设置字段并验证正常身份、分类和交易模式；没有对未配置对象进行交易操作。`Probe` 的隐式复制在编译期不可用，不能用普通复制来证明默认状态已初始化。

```sh
sh src/TestUnits/fixtures/l03_commodity_state/run.sh /tmp/wt-i03-commodity-state-new
```

输出目录必须不存在；普通和 ASan/UBSan/LSan 局部构建各退出 0，日志与当前输入哈希见 `evidence-20261102/`。退出 0 表示观察到当前待修缺陷和显式配置正控，**不能计作修复通过**。ASan/UBSan 不是 MSan，不能检测一般未初始化标量读取；系统和 Boost 依赖并未全部插桩。对齐存储测试不调用公开 `create` 的未配置 getter，也不证明其他平台表现。

解决整项仍需确定未配置对象对现有 getter 的公开语义、明确所有调用者在使用对象前验证配置的准入协议，并选择 ABI 兼容的就绪状态或版本化接口，再验证跨模块和加载路径；单纯指定看似保守的数字、枚举或修改对象布局均不能证明安全。没有进行修复、跨平台或完整回归。

## 零初始化候选的边界

曾对构造函数尝试八个标量置零，但未配置时 `TM_Both` 恰为零，`canShort()` 会返回 true；现有调用方没有统一的 ready 检查。该候选已从产品头文件撤回，必须与未就绪状态及实际交易准入共同处理，不能单独发布。未增加或改变对象字段；在本机 arm64 上修改前后 `sizeof(WTSCommodityInfo)` 均为 632，但这不是跨平台或混合二进制 ABI 验证。

`init.cpp` / `run-init.sh` 保留未来修复的目标探针。它通过 setter 前后的对象表示检查初始化，并在设置过字段后进行 getter 断言，避免读取未初始化标量：

```sh
sh src/TestUnits/fixtures/l03_commodity_state/run-init.sh /tmp/wt-i03-commodity-init-red-20261102
```

当前未修复头文件下，普通与局部 ASan/UBSan/LSan 两个变体均报告 `FAIL: default initialization`，脚本退出 1；不计为通过。另对仅在 `/tmp/wt-i03-commodity-init-candidate/Includes/WTSContractInfo.hpp` 中添加八字段初始化的临时头运行相同的 sanitizer 探针，退出 0；该正控只证明数值初始化和完整配置访问，不证明 ready、缺配置拒绝、交易安全或整体修复。产品头文件未保留该候选。

## 配置加载现状探针

`config_gate.cpp` 使用当前 `WTSBaseDataMgr.cpp`，证明普通商品文件缺失 `pricetick` 或 `volscale` 时，加载器仍返回成功并注册对应字段为零的对象；缺失交易模式则默认 `TM_Both`。通过合约 `rules` 派生品种时也能注册 `pricetick=0` 的商品和合约。另有完整配置正控。普通和局部 ASan/UBSan 各 4/4；其中三个测试通过表示**复现当前缺口**，不是修复验收。旧静态依赖未保证插桩，ASan 禁用 leak detection；未运行真实交易、回放成交或外部 provider。

```sh
bash src/TestUnits/fixtures/l03_commodity_state/run-config.sh /tmp/wt-i03-new084-build
L03_SANITIZE=1 bash src/TestUnits/fixtures/l03_commodity_state/run-config.sh /tmp/wt-i03-new084-build
```

数据导入也服务非交易用途；仅凭这几个诊断不能证明所有零值配置都应全局拒绝。实际 CTA/SEL 回测的成交路径将 `getPriceTick()` 用作除数，live 和 UFT 等交易入口还有不同调用链。实现已裁定的 `ready=false` 需要版本化的可观察状态、字段完整和有限值检查，并在交易/除法前检查；缺失配置对象是否允许只读展示与已有载入兼容必须由这些入口区分。现有 getter 和 `canShort()` 均为头文件内联，旧插件已编译的代码不会自动获得新检查；其版本识别和拒绝需与策略及其他插件能力协商一并落实。当前未修改产品源码或更改既有配置语义。
