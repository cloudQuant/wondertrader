# R60-003：login / connect 准入子项，未完成整体生命周期

在原有 `_request_mutex` 下，`login` 在 release 关闭准入、已 disconnect 或已有 matcher 时返回负值，不再创建线程和投递登录成功；`connect` 检测已有 worker 并拒绝重复启动。连接接口为 void，重复 connect 只写 stderr，不新增 ABI。两者仅防止已列场景替换 joinable 线程和 release 之后重新启动；不改变同实例 release 后拒绝重启的既有行为。

改动前产品文件 SHA256 `bfc386ab3e7932b859930955f838cf8d9619025e06cf2c594e977981a49ae3df`（已含此前未提交的 RAII 和 UDP 修改），本次后 `e45e4f0cd935b5f781f60370206b118c228325c6df7f70ccbc786bb9f02371db`；头文件未动，测试文件改动前 `a242c9a9cd95f49089d82057e554da4746385afe5b38ddeb7ba8935348276524`，本次后 `71ab04a3ca4bda505d56293f07204af707414c17be6ecac3d17ca0a98e22da74`。此前其他脏修改未清理。

旧产品对象与当前测试编译链接的四个红控：release 后 login 及 STOPPING 中 login 的两项失败；重复 login、重复 connect 的两个独立 fork 子进程都进入 terminate handler，退出 88。红控日志及 XML 为 `red-*`、`red2-*`；后两项使用本次修改前单独重编的产品 object 与最终测试 TU，详见命令 JSON。修复后 `final-run.json` 对应八个独立进程定向用例 8/8 PASS，包括上述四项及已有 deferred、引用回收、端口释放、撤单对照；每项独立 25 秒超时。构建独立重编产品和测试 TU，使用实体 Boost bridge，链接复用 `/tmp/ctpm-i03-build` 的库，无法证明完整依赖或最终树一致。命令及输出见对应 `*-commands.json`、`*-run.json`、日志和 XML。

`-fsanitize=address,undefined` 相关 TU 构建成功，但四个目标各自 25 秒超时、无输出；独立最小 `int main(){return 0;}` 在同一 `/usr/bin/c++` sanitizer 环境 8 秒超时（signal 14，`san-control.json`），因此不计 sanitizer 通过，也不能确定目标自身是否另有问题；未运行 TSan。测试不连接真实柜台。

重要未闭合项：release 同实例重启、已接受请求 drain/取消与跨代隔离、回调内 release 的自 join 和 owner 删除/卸载、并发 disconnect / release / login 对线程指针的同步、`isConnected` 并发读取、跨平台和整树回归。此局部修复不代表 R60-003 完成或验收。
