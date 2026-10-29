# 14_ctp 本地 SDK 与调用契约核查

1. `src/API/CTP6.3.15/` 与 `src/API/CTP6.6.9/` 本地包含 CTP trader/market API 头、数据类型/结构头、错误 XML/DTD 与平台动态库/静态库；没有随仓的厂商 C++ 样例或回调语义文档。查看的关键签名为 CTP6.3.15 `ThostFtdcTraderApi.h:74,83,134,161,257`，当前 Apple 平台选择的 CTP6.6.9 对应签名为 `ThostFtdcTraderApi.h:74,83,134,164,176,260`。这些声明不给 `pPayload` / `pRspInfo` 是否可空、查询最后一帧形状、front disconnect 后未决响应顺序或 SPI 注销/Release 回调排空保证。
2. `src/API/CTPOpt3.5.8/` 同样仅有 CTPOpt API 头、数据类型/结构头、错误 XML/DTD 与动态库，没有厂商样例/回调时序文档。`ThostFtdcMdApi.h:92-107,126-128` 只说明 Release、Init、Join、GetTradingDay、RegisterSpi 的声明和简短注释，未说明 RegisterSpi(NULL) / Release 会否等待在途回调。
3. `src/API/XTP2.2.32.2/` 本地包含头文件和动态库，没有匹配版本的厂商字段语义说明。`xquote_api_struct.h:205-213` 仅说明 `pre_settl_price`、`settl_price` 是昨日/今日结算价，未定义 DBL_MAX/FLT_MAX 或其他 sentinel。对该目录执行 `rg -n 'DBL_MAX|FLT_MAX|sentinel|sentinel value|settl_price'` 只命中字段声明，不存在 sentinel 契约文本。
4. 因此本组没有用函数签名推断空末帧一定合法或不合法；凡依赖这些回调序列的条目列为 EXTERNAL_BLOCKED，并写出待取证的 callback 与 payload 形状。stub 只能证明 wrapper 收到人工调用时的本地行为，不能证明 SDK/柜台会产生该序列。
5. 另查标准调用顺序：`src/WtDtServo/ParserAdapter.cpp:149-154,219-225` 在 `connect()` 前注册 SPI 并初始化插件；`WtDtRunner.cpp:378-402` 可由配置创建多个 parser adapter；`ParserAdapterMgr::run` 遍历并启动各实例 (`ParserAdapter.cpp:339-346`)。因此 CTPOpt 的共享可写 `tmp_timestr` 对多个正常配置实例构成线程共享状态风险，和单个 UpdateTime 是否越界是两个不同主张。

本文件只记录本地仓库核查；没有连接真实柜台、构建或执行交易。对父代理提供的公开 PDF URL 尝试使用 web reader 读取，但 reader 返回 inaccessible，未将其内容作为本组证据。

6. 版本对应性补充：`src/TraderCTP/TraderCTP.h:20-24` 在 Apple 平台选择 CTP 6.6.9，在其他平台选择 CTP 6.3.15；`src/TraderCTP/CMakeLists.txt:28-34` 的 Apple 链接也指向 CTP 6.6.9。仓库另有独立的 `CTPMini1.5.8` trader/parser（`src/TraderCTPMini/TraderCTPMini.h:20`、`src/ParserCTPMini/ParserCTPMini.h:13`），没有 `CTPIIMini` 目录或模块。SimNow 提供的 `CTPIIMini_API_Ver1.2.pdf` 产品名/版本与本组 `TraderCTP` 使用的 6.6.9/6.3.15 不同，不能据此将本组 NULL 末帧候选直接升级为 PRODUCT。使用 web reader 打开父代理给出的 PDF URL 未成功；该外部文档内容不作为本组已核实证据。
