# BUG-L00-005 静态 provider 清单

由 `audit_core_providers.py` 只读生成。脚本没有运行 GTest、没有构建或更改 CMake。

- CoverageTests 最终二进制：39 个。
- 四核心归档中重复定义的 `wtp` 类名：50 个。
- 在至少两个核心归档中出现的完全相同 demangled 外部符号：849 个。
- `archive-class-inventory.json` 给出类名、每归档定义符号与精确符号重叠。
- `target-provider-matrix.json` 给出每个最终二进制的核心静态归档链接顺序、全局定义/U 符号、dyld 实际 import image、直接 `.o` 输入中的定义来源候选及二进制 SHA-256。

## 直接 dyld provider 汇总

- `<weak-def-coalesce>`: 3341 个重复类方法/对象 import 项。
- `libWtExecMon`: 62 个重复类方法/对象 import 项。
- `libWtBtPorter`: 3 个重复类方法/对象 import 项。
- `<flat-namespace>`: 1 个重复类方法/对象 import 项。

## 解读边界

`dyld_info` 指向 dylib 的 import 是当前 Mach-O import provider；`nm` 的本地 `T`/`W` 等仅证明符号落在最终 executable image，无法单靠 nm 判断静态归档成员。脚本因此保留链接顺序、精确符号候选与直接对象源路径，并把多候选本地定义标为不可归因；它不把归档重复自动判为错绑。弱合并、flat namespace、未观察到 global symbol、运行时 `dlopen`、以及全 37 类的运行期调用路径均不由该静态矩阵证明。
