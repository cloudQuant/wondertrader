# I03-NEW-101：TraderOES 查询末行转换失败时仍交付最终回调（隔离夹具）

`TraderOES` 不在 CMake 构建中，也没有覆盖 lane。本夹具在 macOS（及 Linux）上直接用仓内 OES 0.17.5.8 头文件编译当前 `src/TraderOES/TraderOES.cpp`，测试可执行文件以“允许未解析符号”方式链接 OES API（`-Wl,-undefined,dynamic_lookup`）。用例从不调用 `init/connect`，`_context` 始终为空，因此不会调用任何 OES API 函数，不连接柜台、不下单。

## 运行

```sh
sh src/TestUnits/fixtures/i03_trader_oes_query_s4/run.sh <新输出目录>
```

可用 `CXX`、`WT_BOOST_INCLUDE`、`WT_GTEST_ROOT` 覆盖默认的 Homebrew 路径。脚本记录源码哈希（`sources.sha256`），每个用例单独进程、单独工作目录、30 秒上限，结果写 `results.txt` 与各用例 `run/<用例>/result.xml`。

`TraderOES.cpp` 自身有 21 条既有告警（未使用参数、成员初始化顺序、未使用私有字段），该文件不在默认 `-Werror` 构建内；脚本对它只记录告警（`trader-oes-warnings.log`、`trader-oes-warning-count.txt`），测试 TU 以 `-Werror` 编译。

## 合同

- 查询游标报告结束（`pQryCursor->isEnd` 或游标为空）时总是交付最终回调，即使最后一行无法转换（原实现直接 `return`，最终回调丢失）。
- 宿主经 `setTraderSpiFeatures` 声明 `WT_TRADER_SPI_FEATURE_QUERY_STATUS` 时，委托查询改走 `onRspOrdersWithStatus`：全部行转换成功为 `COMPLETE`，任一行被丢弃为 `PARTIAL`（K42：宿主不据缺席下结论）；未声明能力的宿主仍收到旧 `onRspOrders`。
- 成交查询按 K42 不做版本化，仍为旧 `onRspTrades`，但末行失败同样交付。
- 无法转换的行仍按 BUG-L01-014 在宿主声明 `WT_TRADER_SPI_FEATURE_RAW_UNRESOLVED` 时上送原始结构。

## 局限

只验证本地受控输入。真实 OES SDK 的游标语义（失败行与 `isEnd` 的组合）、OES 查询失败的错误回调路径、Windows 与真实柜台未验证。
