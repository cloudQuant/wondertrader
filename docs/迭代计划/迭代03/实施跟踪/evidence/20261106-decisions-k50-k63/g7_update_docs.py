#!/usr/bin/env python3
"""Write the K50-K63 outcomes into 执行跟踪.json, 逐项验收.json and 新增问题.md.
Run only after g7_update_tracking.py succeeded (it writes decision-tallies.json)."""
import json, os, sys

G7 = sys.argv[1]
EV = "docs/迭代计划/迭代03/实施跟踪/evidence/20261106-decisions-k50-k63/README.md"
DOC = "/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03"
tallies = json.load(open(os.path.join(G7, "decision-tallies.json")))
FREEZE = "冻结 G7 07e2d89c…（2026-11-06T01:38:48+0800）"

ROWS = {
    "BUG-R211-001": ("已验收", "K54", "active 风控加载失败（缺模块、不兼容）时引擎 riskmon_config_valid=false，WtRunner/WtPorter 初始化失败、run 拒绝；未配置或 active=false 不变；有效模块正控 R110EngineRiskMon", []),
    "BUG-R60-006": ("本地验证通过", "K55", "isConnected 统一为完全就绪：TraderMocker 登录结果送达才就绪，logout/disconnect/release 清零；XTP、YD 等 7 个插件 logout 清除就绪", ["TraderATP/HuaX/HTS/DD/XTPXAlgo 的 logout 改动无厂商头文件，本机未编译运行"]),
    "BUG-R6-006": ("已验收", "K56", "WtPorter 缺 data 段时 config 失败、run 拒绝；data:{} 不配 store 仍合法；缺 env 段对照 R46PorterConfig.MissingEnvMakesConfigFail；显式 release 才停 logger（r82 既有用例）", []),
    "BUG-R134-004": ("已验收", "K57", "实盘不支持外部 tick 加载：非空 tickLoader 告警且不调用；实盘 feed_raw_ticks 打 ERROR 写明丢弃条数", []),
    "BUG-R7-004": ("本地验证通过", "K58", "清仓保守规则：只平可用量、不可用部分保留目标并记诊断、净零锁仓先平可用多头再平可用空头、VWAP 清仓单带平仓标志；股票不卖空（StockVWAP 只卖可用多头）", ["真实柜台的冻结量/T+1 可用量回报与平仓规则"]),
    "BUG-I02-NEW-026": ("已验收", "K59", "StateMonitor 周末规则：只有跨午夜的夜盘延续段看前一天，其余看当天；固定日期驱动 4 组用例；红态为产品偏差（周六白天/晚上与周五节假日夜盘误判为非假日）", []),
    "BUG-I02-NEW-059": ("已验收", "K60", "历史委托队列/明细/成交右端点改为包含（闭区间），与 tick 和实时路径一致；历史与实时全窗口对照一致", []),
    "BUG-I02-NEW-060": ("已验收", "K61", "K 线读端（6 个入口）只支持 m1/m5/day，其他周期返回空并打 ERROR、不读文件不调加载器", []),
    "BUG-I02-NEW-090": ("已验收", "K62", "行覆盖确证 K 线回放的空 tick 列表路径（enqueue 3 次、循环体 0 次）；新增只含空列表的用例与既有空列表+有效 tick 用例均通过", []),
    "BUG-I02-NEW-137": ("本地验证通过", "K63", "文件本身为符号链接的负控在系统临时目录内以 WT_ALLOW_SYMLINK_TESTS=1 单独运行通过，无遗留链接", ["Windows 日志实现为独立分支（无 O_NOFOLLOW），本机未编译运行"]),
}


def apply():
    tp = os.path.join(DOC, "执行跟踪.json")
    d = json.load(open(tp))
    for row in d["rows"]:
        rid = row["id"]
        if rid in ROWS:
            final, k, text, ext = ROWS[rid]
            assert row["状态"] == "阻塞", (rid, row["状态"])
            old = row["状态"]
            row["状态"] = final
            fa = row["最终验收"]
            fa["结论"] = final
            fa["依据"] = ("本机可验证的验收条件全部满足，覆盖用例在最终冻结树通过" if final == "已验收"
                        else "本机部分全部满足；剩余为外部条件或已登记的已知限制")
            fa["冻结"] = FREEZE
            fa["裁定"] = k
            fa["外部剩余"] = ext
            fa["说明"] = f"{k}（用户 2026-11-06 逐项确认）：{text}"
            fa["证据"] = EV
            fa["用例结果"] = {"G7": tallies.get(k, {})}
            row["剩余事项"] = ([f"外部：{e}" for e in ext] if ext else []) + [f"{k} 已实施并验证（2026-11-06），不再按卡15 忽略"]
            row["备注"] = (row.get("备注") or "") + f"\n2026-11-06 用户裁定 {k} 实施（{FREEZE}）：{old} → {final}；{text}。证据 {EV}"
        if rid == "BUG-R18-011":
            row["备注"] += f"\n2026-11-06 用户裁定 K51 实施（{FREEZE}）：ParserShm 检测落后，丢弃最旧数据并告警（WARN 含丢弃条数，交付连续尾部），慢消费者子项完成；外部剩余仍为跨进程多生产者（K21 B）。证据 {EV}"
            row["最终验收"]["说明"] = "K51 已实施（2026-11-06）；外部剩余：跨进程多生产者（K21 B，后续）"
    counts = {}
    for row in d["rows"]:
        counts[row["状态"]] = counts.get(row["状态"], 0) + 1
    assert counts == {"已验收": 148, "本地验证通过": 65}, counts
    old_txt = "已验收 141、本地验证通过 62、待验证 0、阻塞 10（均为卡15 用户指示忽略；T3-015 经 K49=A 转本地验证通过）"
    new_txt = "已验收 148、本地验证通过 65、待验证 0、阻塞 0（2026-11-06 用户逐项确认 K50–K63，原 10 个卡15 阻塞条目按新合同实施并验证）"
    for key in ("当前阶段", "全量回归"):
        if isinstance(d.get(key), str):
            d[key] = d[key].replace(old_txt, new_txt)
    s = json.dumps(d["整体回归验收"], ensure_ascii=False).replace(old_txt, new_txt)
    s = s.replace("本地验证通过 62", "本地验证通过 65").replace("已验收 141", "已验收 148").replace("阻塞 10", "阻塞 0")
    d["整体回归验收"] = json.loads(s)
    d["整体回归验收"]["裁定补充2"] = ("2026-11-06 用户逐项确认 K50–K53 与 K54–K63（原卡15 阻塞条目的合同），全部实施；"
                                  f"最终冻结 G7 07e2d89c… 干净构建与全量逐例回归见 {EV}")
    open(tp, "w").write(json.dumps(d, ensure_ascii=False, indent=2) + "\n")

    zp = os.path.join(DOC, "实施跟踪/evidence/20261105-final-acceptance/逐项验收.json")
    z = json.load(open(zp))
    NEW = {"I03-NEW-089": ("已验收", "K50"), "I03-NEW-042": ("已验收", "K60")}
    for r in z:
        if r["id"] in ROWS:
            r["final_status"] = ROWS[r["id"]][0]
            r["decision"] = ROWS[r["id"]][1] + "（2026-11-06）"
            r["external_pending"] = ROWS[r["id"]][3]
        if r["id"] in NEW:
            r["final_status"] = NEW[r["id"]][0]
            r["decision"] = NEW[r["id"]][1] + "（2026-11-06）"
    open(zp, "w").write(json.dumps(z, ensure_ascii=False, indent=1) + "\n")
    print("tracking updated", counts)


apply()
