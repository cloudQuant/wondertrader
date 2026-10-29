# SSE 股票代码简写合同复核

## 当前规则证据

- `updatelog.md:122-130` 的 0.8.0 更新说明明确写明：股票代码强制标准化为 `SSE.STK.600000`，不再支持 `SSE.600000` 简写。
- 历史提交 `1f5f2b3c687a3393ecdf321932d22cc4b6dc3814` 的提交标题同样说明停止支持 `SSE.600000`；该提交删除 `isSimpStkCode` 和简写映射分支，并将股票解析改为标准三段码。
- 当前 `CodeHelper::extractStdCode` 的注释仍以 `SSE.STK.600000` 为规范示例，见 `src/Share/CodeHelper.hpp:603-606`。
- 当前 `stdCodeToStdCommID2` 的两段码通用分支原样返回输入，见 `src/Share/CodeHelper.hpp:247-263`。文件头 `:245` 保留的 `SSE.600000 -> SSE.STK` 是与 0.8.0 之后合同冲突的旧注释。
- 仓内搜索没有找到 `stdCodeToStdCommID2` 的生产调用点；定义与相关覆盖测试之外无命中。因此目前确认的是公开 helper 行为与旧注释不一致，没有确认仓内生产路径影响。

## 当前测试状态

当前工作树中的 `r12_codehelper_test.cpp:207-215` 已改为验证规范输入 `SSE.STK.600000 -> SSE.STK` 和两段 `SSE.STK` 原样返回。该修改尚未通过本轮重建后的执行验证。更早的 R12 运行针对旧测试期望 `SSE.600000` 原样返回，不应用作当前测试源码的 PASS。
