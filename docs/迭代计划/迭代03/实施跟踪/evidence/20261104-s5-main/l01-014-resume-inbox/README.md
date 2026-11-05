# 卡6 6b=A（BUG-L01-014）：WtRunner 账户恢复的显式命令入口 + 审计

- 新增 `src/WtCore/AccountResumeInbox.hpp`。恢复目录中每个 `<traderId>.json`（字段 `operator`、`reason`、`generation`、`reference`）的处理流程：
  1. 先改名为 `.processing` 认领，再解析，保证只处理一次。
  2. 缺操作员、缺原因、缺审阅代次或代次为 0 的请求不交给门禁，改名为 `.invalid`。
  3. 其余请求交给账户门禁：成功改名为 `.done`，门禁拒绝（代次过期、未暂停、未知交易通道等）改名为 `.rejected`。
  4. 门禁本身写持久审计；入口另记一行结果日志。
  - 操作员身份只记录、不认证；目录写权限即信任边界。
- `WtRunner`：配置项 `resume_folder`，缺省为 `<生成目录>/resume/`。`run()` 同步运行期间约每秒调用一次 `processAccountResumeRequests()`，经 `TraderAdapterMgr::resumeAccountTrading` 恢复。WtPorter 已有 `resume_account_trading` C 接口。
- 用例：`n01_wtcore_tradepaths/s5_main_l01014_resume_inbox_test.cpp`，2 例：
  - 6 个请求按结果分别归档，只处理一次，非 .json 文件不动，目录不存在不报错；
  - 与真实 `AccountTradingGate` 联动：审阅后又发生暂停时，旧代次请求被拒绝且账户保持暂停；按新代次恢复成功，审计日志含 resume 与操作员。
  - 新增入口，无改前版本可对照，红控不适用；两例在当前树通过（`l01014-green.log`）。
- 未覆盖：WtRunner 进程内轮询的端到端用例（需完整运行器与交易通道插件），以及操作员认证、真实 provider、硬断电、Windows。
