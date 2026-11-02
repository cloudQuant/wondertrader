# BUG-T26-002 定向复验

当前 CTPLoader/TraderSpi.cpp、CTPLoader.cpp 及 instrument/flow 两个测试翻译单元以 LLVM22 ASan 和实体 Boost bridge 重编，其他对象和库复用 v8；具体命令和输入哈希分别为 commands.json、sha256.json，复测后无源码漂移见 source-drift.json。七个单例各自独立进程运行，7/7 PASS、无跳过：CZCE 期货、期权/标的、无数字、未终止字段及日期扩年边界。各用例命令、退出码见 results.json 和 run0 至 run6。

ASAN_OPTIONS=detect_leaks=0:halt_on_error=1，仅所述 TU 插桩，LSan 未运行。首次链接选项与 LLVM22 不兼容，最终 commands.json 为修正后的实际命令。本轮未重跑旧源码红控；Windows、真实 SDK/柜台及完整工作树/整体回归未验证。
