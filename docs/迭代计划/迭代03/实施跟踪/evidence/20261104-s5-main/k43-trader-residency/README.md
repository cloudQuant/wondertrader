# 决策 K43（BUG-I02-NEW-052 / I03-NEW-037）：trader provider 与模块常驻到进程退出

裁定（2026-11-04）：无法证明 provider 回调已停止时，TraderAdapter 对象与 DLL 常驻到进程退出，由宿主统一实现。

## 产品
- 新增 `src/Includes/TraderResidency.h`：进程级常驻登记，有意不析构，避免退出时的静态析构顺序问题。
- WtCore 与 WtUftCore 的 `TraderAdapter::release()`：仍调用一次 `ITraderApi::release()`，让 provider 自行停止，并关闭回调准入。provider 一旦获得过回调通道（曾 `registerSpi(this)`，记为 `_spi_ever_registered`），模块工厂创建的 API 就不再 `deleteTrader`，模块也不再 `free_library`，改为登记为常驻并记 info 日志。从未注册 SPI 的 API（例如 init 失败）没有回调来源，仍按原逻辑删除并卸载。UFT 侧原删除分支（含删除器异常、粘滞失败等处理）原样保留。
- 新增 `_module_provider` 标记（init 经模块工厂创建 provider 时置位）与 `hasModuleProvider()`（要求同时曾注册 SPI）。两个 `TraderAdapterMgr::release()` 在清空表之前，把模块适配器对象登记为常驻，因为 provider 可能仍回调其 SPI。
- `initExt`（外部所有的 API）的生命周期不变；TraderDumper 宿主本就不删除、不卸载。
- CRLF 保持；改前/改后副本见 `before/`、`after/`，哈希见 `sources.sha256`。

## 用例（`p11_wtcore_traderadapter/s5_main_k43_trader_residency_test.cpp`，使用真实 `R59CredentialsTrader` 模块，每例单独进程运行）
- `init` + `run` + `release`：常驻 provider 数加 1；`dlopen(RTLD_NOLOAD)` 仍能找到模块（探测增加的引用会立即撤销）。
- 只 `init` 不 `run` 就 `release`：provider 不常驻，模块被卸载（保持原行为）。
- 经 `TraderAdapterMgr::release()`：外部引用释放后，曾 `run` 的适配器对象仍存活。
- 红控：用改前 WtCore `TraderAdapter.{h,cpp}` 重编，两个常驻用例失败（API 被删除、模块被卸载、适配器被析构），“无回调通道”用例改前改后都通过（`k43-red.log`）；恢复改后源码后 3/3 通过（`k43-green2.log`）。
- 按裁定改写 `m_WtUftRunner/r95_wtuftrunner_config_run_test.cpp` 的 `successful_fake_trader_config_registers_and_runs`：正常运行后关停，事件序列止于 `api.release`，不再有 `factory.delete.*`/`api.destroy`。init 失败的两例保持原期望（删除并卸载）。改前副本在 `before/`。int11 中 K43 初版（未区分是否注册过 SPI）使 UFT init 失败回滚 4 例和 r85 3 例失败，据此加入上述区分。
- UFT 侧为同一改动，以 uftcore 逐例回归覆盖，无专门红控。
- 未覆盖：真实 vendor provider 的残留回调、Windows。
