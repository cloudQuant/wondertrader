# I03 策略插件版本化 ABI（卡4b=A / 卡9a）定向夹具

关联：BUG-I02-NEW-002、BUG-L03-002（插件部分）、BUG-R31-002、BUG-I02-NEW-033、BUG-R111-001。只用本地 fake 插件，不连接任何柜台、不下单。

## 内容

- `fake_plugin.cpp`：按 `-DFAKE_FAMILY_{CTA,SEL,HFT,UFT}` 与 `-DFAKE_VARIANT_*` 编出 4 族 × 13 种变体：`V2_OK`（版本化表 + 同步拆除能力）、`V2_NOCAP`（无拆除能力）、`LEGACY`（只有旧无版本导出，按值 deleter）、`LEGACY_BYREF`（历史按引用 deleter 的旧二进制）、`BAD_REVISION`、`BAD_FAMILY`、`BAD_MAJOR`、`NULL_TABLE`、`THROW_GETTER`、`RESERVED`、`NULL_DESTROY`、`SHORT_TABLE`、`NULL_FACTORY`。除 V2_* 外都同时导出旧 creator，用 `fixture_legacy_creates()` 证明宿主是否调用了旧入口。
- `check_live.cpp`：实盘 `CtaStrategyMgr`/`SelStrategyMgr`/`HftStrategyMgr`（直接编译管理器源码 + logger 桩）。
- `check_mocker.cpp`：回测 `CtaMocker`/`SelMocker`/`HftMocker`/`UftMocker::init_*_factory`（链接一棵构建树的 `libWtBtCore.a`、`libWTSTools.a`、`libWTSUtils.a`）。

## 运行

```sh
# 绿：当前源码；红：传入保存的改前管理器源码目录与改前静态库目录
WT_ABI_LIBDIR=<当前树 build_x64/Debug/libs> WT_ABI_RED_LIBDIR=<改前静态库目录> \
  bash src/TestUnits/fixtures/i03_strategy_plugin_abi_20261104s5/run.sh <workdir> <改前管理器源码目录>
```

每个 gtest 用例单独进程、单独 cwd、60 秒 alarm（管理器会常驻已加载模块，计数器不能跨用例共享）。脚本拒绝符号链接；覆盖率插桩的静态库需要 `-fprofile-instr-generate`，`LLVM_PROFILE_FILE` 指向工作目录。

## 合同（由本夹具验证）

1. 宿主只调用本族 getter（`get{Cta,Sel,Hft,Uft}StrategyPluginApiV2`）返回并通过校验的表：主版本 2、族别一致、`object_revision` 与宿主头文件完全相同、保留字段为 0、create/destroy 非空、`struct_size` 覆盖 2.0 布局。
2. 只有旧导出的二进制、以及任何校验失败的表：拒绝，旧 creator/deleter 一次也不调用；实盘管理器返回 false 并在重扫时继续拒绝（模块按既有隔离规则常驻），回测 mocker 返回 false 并卸载模块。
3. 其他族的版本化插件放进本族目录：跳过（实盘 SEL/CTA/HFT 管理器返回值不受影响，回测 mocker 拒绝），不调用其 getter/creator（BUG-R31-002）。
4. 卡9a：只有声明 `WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN` 的插件，宿主才在最后一个策略包装释放后/回测释放时调用 `destroy_factory` 并卸载；无该能力的工厂与模块隔离到进程退出。
