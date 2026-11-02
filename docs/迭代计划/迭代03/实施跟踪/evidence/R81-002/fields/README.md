# BUG-R81-002 字段独立拒绝补充验证

重编当前 CTA 产品 TU 与当前 r42 持久化测试 TU，复用旧 v8 其他对象和库。实际命令及源码/二进制哈希为 commands.json、hashes.json。`R42CtaPersistTest.MalformedSnapshotsDoNotPublishAnyStateOrSubscriptions` 在同一测试内逐项以有效 JSON 基线独立注入 field/alg/action=99，验证拒绝并保留已发布状态；1/1 PASS，XML 与日志在 run/。完整边界见 `/tmp/wt-i03-r81-current/README.md`。
