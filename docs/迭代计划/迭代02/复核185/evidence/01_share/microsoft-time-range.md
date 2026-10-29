# Windows CRT 日期边界核实

核实时间：2026-10-29。仅作为静态平台契约依据；本机是 macOS，未运行 Windows/MSVC。

- Microsoft Learn [`mktime`, `_mktime32`, `_mktime64`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mktime-mktime32-mktime64?view=msvc-170) 的返回值和备注说明：早于 1970-01-01 的日期返回 `-1`；时区换算也可能让边界内日期超出 CRT 可表示范围并返回 `-1`。`mktime` 默认等价于 `_mktime64`，除非定义 `_USE_32BIT_TIME_T`。
- Microsoft Learn [`localtime`, `_localtime32`, `_localtime64`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/localtime-localtime32-localtime64?view=msvc-170) 说明：早于 1970-01-01 的时间返回 `NULL`；负时间会调用 invalid-parameter handler，若 handler 允许继续才返回 `NULL`。
- `TimeUtils::getWeekDay(19691231)` 把该有效公历日期交给 `mktime`，随后未经判错把结果交给 `localtime` 并解引用。Windows CRT 的文档契约说明了有效日期在该平台边界上的失败路径，但没有本机 Windows 运行结果。
- `macos-time-range-probe.json` 是 CST 时区下 Python/macOS libc 的对照探针：1969-12-31 可转换为负时间并成功返回星期信息。它不代表 Windows CRT，也没有替代 MSVC 回归。
