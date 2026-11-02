# BUG-T6-005 两种 loader 的名称提取定向复验

CTPLoader 和 CTPOptLoader 各重新编译当前产品 loader、TraderSpi.cpp、目标测试 TU，共六个编译单元；使用实体 Boost bridge 优先和 LLVM22 `-fsanitize=address,undefined -fno-sanitize-recover=all`。其余对象/静态库/动态库复用迭代02 v8，未全量插桩。`commands.json` 为可逐参数复现的精确编译及链接命令，`sha256.json` 记录输入与二进制，执行后 `drift.json` 无变化。编译使用 `-Wno-error=misleading-indentation`；CTPOptLoader.cpp 既有缩进告警保存在其编译日志中。未修改工作树或创建符号链接。

在两个独立进程，仅运行 `t6_ctp_loader_names.extract_product_name_*`、`CTPOptLoaderCodes.extract_product_name_*`，各 2/2 PASS、无 skip；`results.json`、各自 `run/output.log` 和 `run/result.xml` 记录参数、退出码和 XML。用例覆盖空值、空字符串、ASCII、UTF-8 名称和高位字节；ASan/UBSan 无报告。ASAN_OPTIONS 禁用 LSan，未进行泄漏验证。

本次未重新运行修复前红控；其他平台、真实供应商、完整当前树构建及整体回归均未验证。
