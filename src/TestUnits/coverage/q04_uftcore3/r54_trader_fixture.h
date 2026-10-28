/*!
 * \file r54_trader_fixture.h
 * \brief 第十一波 R54 补测共用替身: 在 q04 纯内存替身之上补一组动作规则矩阵
 *
 * 只复用 q04_trader_fixture.h 的 FakeTraderApi/FakeBaseDataMgr/FakeSink/TempRoot,
 * 不接真实柜台、不装载动态库、不起线程、不开共享内存与 socket。
 */
#pragma once

#include "q04_trader_fixture.h"

namespace r54 {

using namespace wtp;

// 动作规则矩阵: 覆盖 buy/sell 的 AT_CloseToday/AT_CloseYestoday/AT_Close 两种
// CoverMode, 以及开仓的 limit_l/limit_s/limit 三条限额 continue 分支。
inline const char* policyMatrixJson()
{
	return R"JSON({
	"default": {
		"order": [ {"action":"open", "limit":0, "limit_s":0, "limit_l":0, "pure":false} ],
		"filters": []
	},
	"closeshort": {
		"order": [
			{"action":"closeyestoday","limit":0,"limit_s":0,"limit_l":0,"pure":false},
			{"action":"closetoday",   "limit":0,"limit_s":0,"limit_l":0,"pure":false}
		],
		"filters": ["SHFE.ni"]
	},
	"closeshort2": {
		"order": [ {"action":"close","limit":0,"limit_s":0,"limit_l":0,"pure":false} ],
		"filters": ["SHFE.ni2"]
	},
	"closecu": {
		"order": [ {"action":"close","limit":0,"limit_s":0,"limit_l":0,"pure":false} ],
		"filters": ["SHFE.cu"]
	},
	"closelong": {
		"order": [
			{"action":"closeyestoday","limit":0,"limit_s":0,"limit_l":0,"pure":false},
			{"action":"closetoday",   "limit":0,"limit_s":0,"limit_l":0,"pure":false}
		],
		"filters": ["SHFE.ao"]
	},
	"closelong2": {
		"order": [ {"action":"close","limit":0,"limit_s":0,"limit_l":0,"pure":false} ],
		"filters": ["SHFE.ao2"]
	},
	"closecu2": {
		"order": [ {"action":"close","limit":0,"limit_s":0,"limit_l":0,"pure":false} ],
		"filters": ["SHFE.cu2"]
	},
	"closelongpure": {
		"order": [
			{"action":"closeyestoday","limit":0,"limit_s":0,"limit_l":0,"pure":true},
			{"action":"closetoday",   "limit":0,"limit_s":0,"limit_l":0,"pure":true}
		],
		"filters": ["SHFE.ap"]
	},
	"longlimit": {
		"order": [ {"action":"open","limit":0,"limit_s":0,"limit_l":2,"pure":false} ],
		"filters": ["SHFE.zn"]
	},
	"totallimit": {
		"order": [ {"action":"open","limit":3,"limit_s":0,"limit_l":0,"pure":false} ],
		"filters": ["SHFE.pb"]
	},
	"shortlimit": {
		"order": [ {"action":"open","limit":0,"limit_s":2,"limit_l":0,"pure":false} ],
		"filters": ["SHFE.sn"]
	}
})JSON";
}

struct Env
{
	q04::TempRoot			tmp{ "r54_trader" };
	q04::FakeTraderApi		api;
	q04::FakeBaseDataMgr	bd;
	ActionPolicyMgr			policy;
	q04::FakeSink			sink;
	TraderAdapter			adapter;
	std::string				id;

	explicit Env(const char* channel, const char* policyJson = nullptr)
		: id(channel)
	{
		if (policyJson != nullptr)
		{
			const std::string path = tmp.file("policy.json");
			tmp.write(path, policyJson);
			EXPECT_TRUE(policy.init(path.c_str()));
		}

		EXPECT_TRUE(adapter.initExt(id.c_str(), &api, &bd, &policy));
		adapter.addSink(&sink);
		EXPECT_TRUE(adapter.run());
		adapter.onLoginResult(true, "ok", 20240315);
	}

	WTSContractInfo* add(const char* exchg, const char* pid, const char* code,
		uint32_t maxMktVol = 0, uint32_t maxLmtVol = 0,
		CoverMode cm = CM_OpenCover, TradingMode tm = TM_Both)
	{
		return bd.add(exchg, pid, code, maxMktVol, maxLmtVol, cm, tm);
	}

	void loadPosition(const char* exchg, const char* code,
		double lPre, double lPreAvail, double lNew, double lNewAvail,
		double sPre, double sPreAvail, double sNew, double sNewAvail)
	{
		std::vector<WTSObject*> items{
			q04::posItem(exchg, code, WDT_LONG, lPre, lPreAvail, lNew, lNewAvail),
			q04::posItem(exchg, code, WDT_SHORT, sPre, sPreAvail, sNew, sNewAvail),
		};
		WTSArray* ay = WTSArray::create();
		for (auto one : items)
			ay->append(one);
		adapter.onRspPosition(ay);
		ay->release();
		for (auto one : items)
			one->release();
	}

	// 成交回报: 必须挂上 ContractInfo, 否则 TraderAdapter::onRspTrades 会整条跳过
	void feedTrade(const char* exchg, const char* code, WTSDirectionType dir,
		WTSOffsetType offset, double vol)
	{
		WTSTradeInfo* t = WTSTradeInfo::create(code, exchg);
		t->setContractInfo(bd.getContract(code, exchg));
		t->setDirection(dir);
		t->setOffsetType(offset);
		t->setVolume(vol);
		t->setPrice(5000.0);
		WTSArray* ay = WTSArray::create();
		ay->append(t);
		adapter.onRspTrades(ay);
		ay->release();
		t->release();
	}

	uint32_t place(double price, double qty, bool isLong, bool isOpen, bool isToday = false)
	{
		uint32_t localid;
		if (isOpen)
			localid = isLong ? adapter.openLong("SHFE.ag2406", price, qty, 0)
				: adapter.openShort("SHFE.ag2406", price, qty, 0);
		else
			localid = isLong ? adapter.closeLong("SHFE.ag2406", price, qty, isToday, 0)
				: adapter.closeShort("SHFE.ag2406", price, qty, isToday, 0);
		EXPECT_NE(UINT_MAX, localid);
		return localid;
	}

	const q04::FakeTraderApi::EntrustSnap& lastSnap() const
	{
		return api._entrusts.back();
	}

	// 按最近一笔委托组装实时委托回报并推送
	WTSOrderInfo* pushLast(const char* orderid, WTSOrderState st, WTSOffsetType offset,
		double volTraded = 0.0, WTSOrderFlag flag = WOF_NOR)
	{
		const q04::FakeTraderApi::EntrustSnap& snap = api._entrusts.back();
		WTSOrderInfo* ord = q04::orderFromSnap(snap, orderid);
		ord->setOrderState(st);
		ord->setOffsetType(offset);
		ord->setOrderFlag(flag);
		ord->setVolTraded(volTraded);
		ord->setVolLeft(snap._vol - volTraded);
		adapter.onPushOrder(ord);
		return ord;
	}
};

}	// namespace r54
