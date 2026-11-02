# T2-CTP-07 CTP 未触发状态映射子项

CTP 6.6.9 `NotTouched` 是字符 `b`，WTS 中 `WOS_Cancelling` 为 `b`、`WOS_Nottouched` 为 `c`；直接数值转换误报撤单中。现明确映射 NotTouched 到 WOS_Nottouched。查询和推送假回报入口的新测试及其他状态正控 5/5 PASS；旧产品对象在两个新矩阵用例均失败，详见 results.json、run0 至 run6。当前源码与新测试 TU 重编，其他对象复用 v8；编译/链接命令及源码指纹见 build.log 和 sha256.txt。

CTP `Touched`（`c`）依旧强转并落入 WOS_Nottouched，WTS 如何表达已触发状态尚未裁定；整条保持阻塞。验证限 macOS/CTP 6.6.9 fake 回报，其他版本、Windows/Linux、真实 provider 和整体回归未验。
