# 迭代03 卡12 容器所有权定向验证（BUG-R53-002 / BUG-R12-14）

用户裁定（2026-11-03）：12a=A，12b=C。

- `WTSMap::erase` 的 Iterator、ConstIterator、key 三个重载都会先摘除节点，再释放容器持有的那份引用，和 `remove`/`clear` 一致。key 不存在时什么也不做；值为 NULL 时可以安全调用。
- `WTSStack` 新增三个显式入口：`pushOwned`（栈自己 retain 一份）、`pushTransferred`（调用方把已有的一份引用交给栈）、`pushBorrowed`（只借用，clear 和析构都不释放），以及 `isTopBorrowed()`。旧入口 `push(obj, true)` 等同 `pushOwned`；`push(obj, false)` 为兼容旧代码按所有权转移处理，等同 `pushTransferred`。`pop()` 总是返回一份调用方拥有、必须且只能 release 一次的引用：拥有的元素直接交出栈持有的引用，借用的元素先 retain 再返回。`top()` 返回借用指针。
- 头文件定义了 `WTS_COLLECTION_OWNERSHIP_CONTRACT 20261103L`，插件可以用它判断自己编译时用的是哪一版语义。

## 文件

- `check.cpp`：用 `Probe` 统计构造和析构次数。每个 case 都断言精确的引用计数，结束时检查构造数等于析构数。可以单个 case 运行（`check <case>`），也可以 `check list` 或 `check all`。新 API 的 case 只有在头文件定义了上述宏时才会编译。
- `baseline/Includes/WTSCollection.hpp`：HEAD `ccaad010` 的原始头文件（sha256 `b932b4cd…0476`），用于 red 对照。
- `baseline/tests/`：改 oracle 之前的 R12/R53 lane 用例快照，用于证明旧 oracle 编码的正是已被否决的语义。
- `run.sh`：用 Homebrew LLVM 22.1.8 编译，开启 `-fsanitize=address,undefined`、`detect_leaks=1` 和 `-Wall -Wextra -Werror`，依次产出：
  1. `check-green`（当前头）和 `check-red`（baseline 头），每个 case 单独一个进程，结果写入 `*.results.tsv`；
  2. `lane-green`：用 vendored gtest 单独编译现有的 `r12_collection_test.cpp` 和 `r53_collection_variant_test.cpp`，只依赖头文件，不链接任何 core 静态库；
  3. `lane-oldoracle-newheader`：改动前的 lane 用例对新头运行。

```sh
sh src/TestUnits/fixtures/i03_container_ownership_20261103/run.sh /tmp/wt-i03-container-20261103-asan/<新目录>
```

## 局限

- 本目录只做头文件级别的定向验证，不是全量回归，也没有构建 CoverageTests 主目标。
- 用旧头编译的外部插件二进制保留旧语义。WTSStack 的内部布局已经改变，跨 DSO 共享 WTSStack 对象或混用 inline 定义都没有验证过。Windows/MSVC 也没有验证。
