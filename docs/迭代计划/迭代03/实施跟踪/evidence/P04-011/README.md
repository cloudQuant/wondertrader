# BUG-P04-011 外部 Tick loader 数据生命周期子项

现有 loader 合同要求 loadRawHisTicks 返回前完成回调，回调提供的数据只在回调期间有效。新增 fake loader 将各分块在 callback 返回后 volatile 覆写并销毁，串行和并发分块均在 load 返回前 join；连同空回调、非法 payload、loader false 和既有多块用例，普通和严格 ASan 最小链接各 7/7 PASS，见 `normal-final.xml` 和 `asan-final.xml`。只修改测试，产品已有 callback 内复制逻辑。

最初 LLVM22 插桩对象与 v8 旧对象混链发生 ASan container-overflow（`asan-strict.log`）；保持同一组对象和 ASan 配置仅调整链接顺序后，对应用例通过。两份压缩链接 map 表明未带 ABI 标签的 vector 弱符号来源不同。最小链接将新插桩 test/replayer 放前，未关闭 container-overflow；commands JSON 记录构建。该诊断不证明其他内存错误不存在。只有测试与 replayer TU 插桩，其他库和对象为旧构建，LSan 关闭。

真实外部 provider 的同步合同、同刻并发顺序、跨 session/品种的回放时钟和整体回归均未验；这不是 P04-011 整体完成。`source-sha256.txt` 是证据保存时相关文件散列。
