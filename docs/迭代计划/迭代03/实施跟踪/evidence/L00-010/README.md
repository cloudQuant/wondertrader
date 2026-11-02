# BUG-L00-010 定向证据

- 主代理基于当前源码执行：`/usr/bin/clang++ -std=c++17 -Wall -Wextra -Werror -Isrc/TestUnits/fixtures/module_helper_odr/windows/fake_api -Isrc src/TestUnits/fixtures/module_helper_odr/windows/fake_api_test.cpp -o /tmp/wt-i03-l00-010-check/windows-fake-api-primary`，然后逐进程执行 normal、long、handle_failure、null_handle、file_failure、no_separator、always_truncated；7/7 退出 0。源码和二进制哈希见 sha256.txt。这是 macOS 上的 Windows API 替身，不是 Windows/MSVC 集成测试。
- 复用执行器编译的 macOS 双 TU 动态库及探针 `/tmp/wt-i03-l00-010-check/posix-test /tmp/wt-i03-l00-010-check/posix-lib/module-helper.dylib`，退出 0；探针对 POSIX `getInstPath()`，不验证 Windows 代码。
- 新增的 Windows 双 DLL、多 TU、静态初始化和重载 fixture 尚未在 Windows/MSVC 编译或运行。本机不具该环境；该项保持阻塞。Windows 查询失败返回空目录，但部分调用方会把它与库名直接拼接，可能触发相对路径加载；需复核下游错误处理。
