# BUG-I02-NEW-053 定向复验（2026-11-01）

结论：四项组合的 GoogleTest 断言 4/4 通过，但开启独立 LLVM_PROFILE_FILE 的严格 LSan 进程退出码为 23，报告 48 字节 strdup 泄漏，故该次严格检查未通过。相同二进制及相同严格 sanitizer 设置、仅取消 LLVM_PROFILE_FILE 的对照结果为 4/4、退出 0。独立未知合约用例 1/1、退出 0。没有移除断言或添加 LSan 抑制；结果不能泛化为全量泄漏验收。

## 工具链和构建

macOS 本机，LLVM 22.1.8 clang++。使用 `/tmp/wt-i02-fresh-20261101-v8-build` 既有编译图和库，其中 Boost bridge 为实体目录；当前仓库的 TraderAdapter.cpp、p11_usertag_bounds_test.cpp、r59_trader_misc_test.cpp 三个翻译单元以 `-Werror -fsanitize=address,undefined -fno-omit-frame-pointer` 重编，其余产品对象、依赖库和动态测试插件复用 v8 构建并非全部重新插桩。重建链接可执行文件 `/tmp/wt-i03-new053/bin/fixture-asan`；没有创建符号链接或修改仓库。

精确构建命令在 `commands.json`，每个数组是一条命令的 argv，cwd 均为 `/tmp/wt-i02-fresh-20261101-v8-build`。脚本 `build.py` 记录由 Ninja 命令生成它们的过程；四步 `build-0.log` 至 `build-3.log` 均退出 0。第一次编译未配置 stdlib 和 RapidJSON include 而失败，原始失败保存在 `build-initial-failure.log` 及 `commands-initial-failure.json`；随后加 `-include stdlib.h -isystem /usr/local/include` 重建成功。编译前后当前源码、头文件 SHA-256 在 `source-hashes.json`，构建期间一致；`artifact-hashes.txt` 固定可执行文件及插件 SHA-256。

## 测试运行与原始证据

依次运行以下命令，Python 均使用用户 conda base。脚本指定独立 cwd、TMPDIR 和 45 秒超时，并保存精确测试 argv、过滤器、退出码、XML 与原始输出；`run.py` 和 `run-individual.py` 设置 `LLVM_PROFILE_FILE=<各自运行目录>/profile-%p.profraw`，`no-profile-control.py` 将其从继承环境删除。

```shell
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-new053/run.py
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-new053/run-individual.py
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-new053/no-profile-control.py
```

所有运行统一设置 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`LSAN_OPTIONS=exitcode=23:report_objects=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`；未使用 suppression。`results.json`、`individual-results.json`、`group-no-profile-result.json` 保存各次精确 argv 和退出码。每个运行目录的 `output.log` 为 GoogleTest/LSan 原始合并输出，`result.xml` 为原始结果。

| 运行目录 | 用例 | GoogleTest 结果 | 进程结果 |
|---|---|---|---|
| run/group | P11 三项 + R59Misc.init_ext_guards_and_run_without_api | 4/4 | exit 23，LSan 48 字节 |
| run/single0 | P11TraderTest.init_rejects_overlong_ids_and_keeps_empty_id_accepted | 1/1 | exit 23，LSan 50 字节 |
| run/single1 | P11TraderTest.init_ext_rejects_long_ids_before_touching_api_or_files | 1/1 | exit 0 |
| run/single2 | P11TraderTest.longest_representable_id_produces_a_terminated_user_tag | 1/1 | exit 0 |
| run/single3 | R59Misc.init_ext_guards_and_run_without_api | 1/1 | exit 0 |
| run/group_no_profile | 同四项，不设置 LLVM_PROFILE_FILE | 4/4 | exit 0，未见 LSan 报告 |
| run/missing | R59Misc.push_trade_without_contract_is_ignored | 1/1 | exit 0，未见 LSan 报告 |

报告栈指向 `DLLHelper::load_library -> TraderAdapter::init` 加载的动态模块初始化中的 strdup，模块在报告时已卸载，无法准确符号化到分配者。两次报告大小分别恰与各自 profile 路径字符串（含 NUL）的长度吻合；该插件含 LLVM_PROFILE_FILE、LLVM profile 初始化和 strdup 符号（见 `plugin-strings.txt`、`plugin-symbols.txt`）。与取消变量的对照一起强烈提示覆盖率运行时及动态模块生命周期相关，但并不能证明确切根因，也不能抹去严格 LSan 的失败证据。

本结果不改变关联历史组 45/53、R59Exec abort 或产品泄漏 BUG-I02-NEW-052 的独立未关闭状态；没有运行这些其他用例。未证明全部产品对象、其他系统平台或完整 5000+ 测试的行为。
