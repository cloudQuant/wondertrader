// L03 Includes lane 单元测试：WTSContractInfo 合约信息
// 目标文件: src/Includes/WTSContractInfo.hpp (L168-L357)
// 注意：m_maxMktQty/m_maxLmtQty/m_minMktQty/m_minLmtQty/m_strAltCode 无初始化，
// 读取前必须先写入（见 bugs/l03_includes.md BUG-L03-002）。
#include <unistd.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSMarcos.h"
#include "WTSTools/WTSBaseDataMgr.h"

using namespace wtp;

namespace
{
WTSCommodityInfo* mkComm(uint32_t volScale = 10, double marginRate = 0.1)
{
	WTSCommodityInfo* c = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "NIGHT_DAY", "CHN");
	c->setVolScale(volScale);
	c->setPriceTick(1.0);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Future);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Both);
	c->setMarginRate(marginRate);
	return c;
}

WTSContractInfo* mkContract(const char* code = "rb2401", const char* pid = "rb")
{
	WTSContractInfo* ret = WTSContractInfo::create(code, "螺纹钢2401", "SHFE", pid);
	ret->setVolScale(10);
	ret->setVolumeLimits(500, 500);
	return ret;
}

std::filesystem::path writeIdentityConfig(const std::string& body)
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto path = std::filesystem::temp_directory_path() /
		("wt_l03_identity_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(stamp) + ".json");
	std::ofstream output(path);
	output << body;
	output.close();
	return path;
}

//////////////////////////////////////////////////////////////////////////
// create / 默认值
//////////////////////////////////////////////////////////////////////////
TEST(l03_contract, create_and_defaults)
{
	WTSContractInfo* c = mkContract();
	ASSERT_NE(c, nullptr);
	EXPECT_STREQ(c->getCode(), "rb2401");
	EXPECT_STREQ(c->getName(), "螺纹钢2401");
	EXPECT_STREQ(c->getExchg(), "SHFE");
	EXPECT_STREQ(c->getProduct(), "rb");
	EXPECT_STREQ(c->getFullCode(), "SHFE.rb2401");
	EXPECT_STREQ(c->getFullPid(), "SHFE.rb");
	// 上市/交割日有构造函数初始化列表，可以安全断言默认值
	EXPECT_EQ(c->getOpenDate(), 19900101u);
	EXPECT_EQ(c->getExpireDate(), 30991231u);
	// 全局索引默认 UINT_MAX
	EXPECT_EQ(c->getTotalIndex(), UINT_MAX);
	EXPECT_EQ(c->getVolScale(), 10u);
	// 未挂接品种信息
	EXPECT_EQ(c->getCommInfo(), nullptr);
	// 主力/连续合约列表默认为空
	EXPECT_TRUE(c->isFlat());
	EXPECT_TRUE(c->getHotCodes().empty());
	// 扩展数据指针默认清零，可安全读取
	EXPECT_EQ(c->getExtData<void*>(), nullptr);
	c->release();
}

TEST(l03_contract, date_and_limit_setters)
{
	WTSContractInfo* c = mkContract("rb2410");
	c->setDates(20231010, 20241010);
	EXPECT_EQ(c->getOpenDate(), 20231010u);
	EXPECT_EQ(c->getExpireDate(), 20241010u);

	// 显式 4 个参数
	c->setVolumeLimits(8000, 500, 2, 1);
	EXPECT_EQ(c->getMaxMktVol(), 8000u);
	EXPECT_EQ(c->getMaxLmtVol(), 500u);
	EXPECT_EQ(c->getMinMktVol(), 2u);
	EXPECT_EQ(c->getMinLmtVol(), 1u);

	// 默认参数（min 都是 1）
	c->setVolumeLimits(100, 200);
	EXPECT_EQ(c->getMinMktVol(), 1u);
	EXPECT_EQ(c->getMinLmtVol(), 1u);
	// 上限可以是 0，接口不做合法性校验
	c->setVolumeLimits(0, 0, 0, 0);
	EXPECT_EQ(c->getMaxMktVol(), 0u);
	EXPECT_EQ(c->getMinLmtVol(), 0u);
	c->release();
}

TEST(l03_contract, alt_code_for_zce)
{
	WTSContractInfo* c = mkContract("au2402", "au");
	// 郑商所风格：合约代码 3 位，别称 4 位
	c->setAltCode("au2402");
	EXPECT_STREQ(c->getAltCode(), "au2402");
	EXPECT_STREQ(c->getFullAltCode(), "SHFE.au2402");
	// 覆盖写入更短的别称不会残留旧字符
	c->setAltCode("au001");
	EXPECT_STREQ(c->getAltCode(), "au001");
	EXPECT_STREQ(c->getFullAltCode(), "SHFE.au001");
	// 空别称
	c->setAltCode("");
	EXPECT_STREQ(c->getAltCode(), "");
	EXPECT_STREQ(c->getFullAltCode(), "SHFE.");
	c->release();
}

TEST(l03_contract, identity_fields_reject_values_that_do_not_fit)
{
	const std::array<std::size_t, 4> capacities = { 32, 64, 32, 32 };
	const std::array<const char*, 4> base = { "rb2401", "name", "SHFE", "rb" };
	auto create = [](const std::array<const char*, 4>& fields) {
		return WTSContractInfo::create(fields[0], fields[1], fields[2], fields[3]);
	};

	for (std::size_t field = 0; field < base.size(); ++field)
	{
		const std::array<std::size_t, 3> lengths = {
			capacities[field] - 1, capacities[field], capacities[field] + 1
		};
		for (std::size_t delta = 0; delta < lengths.size(); ++delta)
		{
			const std::string value(lengths[delta], 'x');
			auto values = base;
			values[field] = value.c_str();
			WTSContractInfo* info = create(values);
			if (delta == 0)
			{
				ASSERT_NE(nullptr, info) << "field " << field;
				const std::array<const char*, 4> actual = {
					info->getCode(), info->getName(), info->getExchg(), info->getProduct()
				};
				EXPECT_STREQ(value.c_str(), actual[field]) << "field " << field;
				info->release();
			}
			else
			{
				EXPECT_EQ(nullptr, info) << "field " << field << ", length " << lengths[delta];
			}
		}
	}

	EXPECT_EQ(nullptr, WTSContractInfo::create(nullptr, "name", "SHFE", "rb"));
	WTSContractInfo* empty = WTSContractInfo::create("", "", "", "");
	ASSERT_NE(nullptr, empty);
	EXPECT_STREQ("", empty->getCode());
	EXPECT_STREQ("", empty->getName());
	EXPECT_STREQ("", empty->getExchg());
	EXPECT_STREQ("", empty->getProduct());
	empty->release();

	const std::string maxContractPart(31, 'z');
	WTSContractInfo* composite = WTSContractInfo::create(
		maxContractPart.c_str(), std::string(63, 'n').c_str(), maxContractPart.c_str(), maxContractPart.c_str());
	ASSERT_NE(nullptr, composite);
	EXPECT_EQ(63u, std::strlen(composite->getFullCode()));
	EXPECT_EQ(63u, std::strlen(composite->getFullPid()));
	ASSERT_TRUE(composite->setAltCode(maxContractPart.c_str()));
	EXPECT_EQ(63u, std::strlen(composite->getFullAltCode()));
	composite->release();
}

TEST(l03_contract, alt_code_capacity_rejection_preserves_previous_identity)
{
	WTSContractInfo* c = mkContract();
	ASSERT_TRUE(c->setAltCode("rb2401"));
	EXPECT_STREQ("rb2401", c->getAltCode());
	EXPECT_STREQ("SHFE.rb2401", c->getFullAltCode());

	EXPECT_TRUE(c->setAltCode(std::string(31, 'a').c_str()));
	EXPECT_EQ(31u, std::strlen(c->getAltCode()));
	EXPECT_STREQ(std::string(31, 'a').c_str(), c->getAltCode());
	EXPECT_EQ(36u, std::strlen(c->getFullAltCode()));
	const std::string prior = c->getAltCode();
	const std::string priorFull = c->getFullAltCode();
	EXPECT_FALSE(c->setAltCode(std::string(32, 'b').c_str()));
	EXPECT_STREQ(prior.c_str(), c->getAltCode());
	EXPECT_STREQ(priorFull.c_str(), c->getFullAltCode());
	EXPECT_FALSE(c->setAltCode(std::string(33, 'c').c_str()));
	EXPECT_STREQ(prior.c_str(), c->getAltCode());
	EXPECT_STREQ(priorFull.c_str(), c->getFullAltCode());
	EXPECT_FALSE(c->setAltCode(nullptr));
	c->release();
}

TEST(l03_contract, oversized_configured_identity_is_rejected_before_registration)
{
	const std::string longProduct(64, 'p');
	const auto commodityPath = writeIdentityConfig(
		"{\"SHFE\":{\"" + longProduct + "\":{\"name\":\"product\",\"session\":\"ALLDAY\"}}}");
	WTSBaseDataMgr commodityMgr;
	EXPECT_FALSE(commodityMgr.loadCommodities(commodityPath.string().c_str()));
	EXPECT_EQ(nullptr, commodityMgr.getCommodity(("SHFE." + longProduct).c_str()));
	std::filesystem::remove(commodityPath);

	const std::string longCode(32, 'c');
	const auto contractPath = writeIdentityConfig(
		"{\"SHFE\":{\"" + longCode +
		"\":{\"name\":\"contract\",\"exchg\":\"SHFE\",\"rules\":{\"session\":\"ALLDAY\"}}}}");
	WTSBaseDataMgr contractMgr;
	EXPECT_FALSE(contractMgr.loadContracts(contractPath.string().c_str()));
	EXPECT_EQ(nullptr, contractMgr.getContract(longCode.c_str(), "SHFE"));
	EXPECT_EQ(nullptr, contractMgr.getCommodity("SHFE", longCode.c_str()));
	std::filesystem::remove(contractPath);

	const std::string longAltCode(32, 'a');
	const auto altPath = writeIdentityConfig(
		"{\"SHFE\":{\"rb2401\":{\"name\":\"contract\",\"exchg\":\"SHFE\",\"product\":\"rb\",\"altcode\":\"" +
		longAltCode + "\"}}}");
	WTSBaseDataMgr altMgr;
	EXPECT_FALSE(altMgr.loadContracts(altPath.string().c_str()));
	EXPECT_EQ(nullptr, altMgr.getContract("rb2401", "SHFE"));
	std::filesystem::remove(altPath);
}

// 2026-11-04 (BUG-L03-002, decision card 4a=A): rules-derived commodities are
// registered only when ready (positive pricetick/volscale and a loaded
// session), and a file with any rejected entry is not published at all. The
// original oracle used rules without pricetick/volscale, never loaded the
// ALLDAY session, and expected the valid half of a mixed file to be
// registered; those three expectations contradict the decided contract. The
// width limits themselves are unchanged: every invalid case below is
// otherwise ready, so width is its only reason for rejection.
TEST(l03_contract, storage_width_contract_limits_preserve_valid_edges_and_reject_before_rules_registration)
{
	const std::string validExchg(MAX_EXCHANGE_LENGTH - 1, 'E');
	const std::string longExchg(MAX_EXCHANGE_LENGTH, 'X');
	const std::string validCode(MAX_INSTRUMENT_LENGTH - 1, 'C');
	const std::string longCode(MAX_INSTRUMENT_LENGTH, 'D');
	const std::string readyRules =
		"\"rules\":{\"session\":\"ALLDAY\",\"pricetick\":1,\"volscale\":1}";
	auto body = [&readyRules](const std::string& group, const std::string& exchg, const std::string& code) {
		return "{\"" + group + "\":{\"" + code +
			"\":{\"name\":\"contract\",\"exchg\":\"" + exchg +
			"\"," + readyRules + "}}}";
	};
	const auto sessionFile = writeIdentityConfig(
		"{\"ALLDAY\":{\"name\":\"all day\",\"offset\":0,\"sections\":[{\"from\":0,\"to\":0}]}}");
	auto loadSession = [&sessionFile](WTSBaseDataMgr& mgr) {
		return mgr.loadSessions(sessionFile.string().c_str());
	};

	const auto validFile = writeIdentityConfig(body(validExchg, validExchg, validCode));
	WTSBaseDataMgr validMgr;
	ASSERT_TRUE(loadSession(validMgr));
	ASSERT_TRUE(validMgr.loadContracts(validFile.string().c_str()));
	WTSContractInfo* valid = validMgr.getContract(validCode.c_str(), validExchg.c_str());
	ASSERT_NE(nullptr, valid);
	EXPECT_STREQ(validExchg.c_str(), valid->getExchg());
	EXPECT_STREQ(validCode.c_str(), valid->getCode());
	EXPECT_NE(nullptr, validMgr.getCommodity(validExchg.c_str(), validCode.c_str()));
	EXPECT_EQ(1u, validMgr.getGlobalSize());
	std::filesystem::remove(validFile);

	struct Case { std::string group, exchg, code; };
	const std::vector<Case> invalid = {
		{longExchg, longExchg, "ABC"},
		{longExchg, validExchg, "DEF"},
		{validExchg, longExchg, "GHI"},
		{validExchg, validExchg, longCode}
	};
	for (const auto& entry : invalid)
	{
		const auto file = writeIdentityConfig(body(entry.group, entry.exchg, entry.code));
		WTSBaseDataMgr mgr;
		ASSERT_TRUE(loadSession(mgr));
		EXPECT_FALSE(mgr.loadContracts(file.string().c_str()));
		EXPECT_EQ(nullptr, mgr.getContract(entry.code.c_str(), entry.exchg.c_str()));
		EXPECT_EQ(nullptr, mgr.getCommodity(entry.group.c_str(), entry.code.c_str()));
		EXPECT_EQ(0u, mgr.getGlobalSize());
		std::filesystem::remove(file);
	}
	const std::string mixed = "{\"" + validExchg + "\":{\"" + validCode +
		"\":{\"name\":\"good\",\"exchg\":\"" + validExchg +
		"\"," + readyRules + "},\"" + longCode +
		"\":{\"name\":\"bad\",\"exchg\":\"" + validExchg +
		"\"," + readyRules + "}}}";
	const auto mixedFile = writeIdentityConfig(mixed);
	WTSBaseDataMgr mixedMgr;
	ASSERT_TRUE(loadSession(mixedMgr));
	EXPECT_FALSE(mixedMgr.loadContracts(mixedFile.string().c_str()));
	// Card 4a=A: one rejected entry keeps the whole file unpublished.
	EXPECT_EQ(nullptr, mixedMgr.getContract(validCode.c_str(), validExchg.c_str()));
	EXPECT_EQ(nullptr, mixedMgr.getCommodity(validExchg.c_str(), validCode.c_str()));
	EXPECT_EQ(nullptr, mixedMgr.getContract(longCode.c_str(), validExchg.c_str()));
	EXPECT_EQ(nullptr, mixedMgr.getCommodity(validExchg.c_str(), longCode.c_str()));
	EXPECT_EQ(0u, mixedMgr.getGlobalSize());
	std::filesystem::remove(mixedFile);
	std::filesystem::remove(sessionFile);
}

//////////////////////////////////////////////////////////////////////////
// 品种挂接与保证金率解析
//////////////////////////////////////////////////////////////////////////
TEST(l03_contract, margin_ratio_from_commodity)
{
	WTSCommodityInfo* comm = mkComm();
	WTSContractInfo* c = mkContract();
	c->setCommInfo(comm);
	ASSERT_EQ(c->getCommInfo(), comm);

	// flag == 1：使用合约手工设置值
	c->setMarginRatios(0.08, 0.12, 1);
	EXPECT_DOUBLE_EQ(c->getLongMarginRatio(), 0.08);
	EXPECT_DOUBLE_EQ(c->getShortMarginRatio(), 0.12);

	// flag == 0 且品种保证金率非 0：取品种保证金率
	c->setMarginRatios(0.08, 0.12, 0);
	EXPECT_DOUBLE_EQ(c->getLongMarginRatio(), 0.1);
	EXPECT_DOUBLE_EQ(c->getShortMarginRatio(), 0.1);

	// flag == 0 且品种保证金率为 0：回落到合约自身值
	comm->setMarginRate(0.0);
	EXPECT_DOUBLE_EQ(c->getLongMarginRatio(), 0.08);
	EXPECT_DOUBLE_EQ(c->getShortMarginRatio(), 0.12);

	// 默认 flag 参数是 0
	c->setMarginRatios(0.2, 0.3);
	EXPECT_DOUBLE_EQ(c->getLongMarginRatio(), 0.2);
	c->release();
	comm->release();
}

TEST(l03_contract, calc_fee_delegates_to_commodity)
{
	WTSCommodityInfo* comm = mkComm(10);
	comm->setFeeRates(3.0, 4.0, 5.0, true);
	WTSContractInfo* c = mkContract();
	c->setCommInfo(comm);
	// 合约未配置手续费率（m_nFeeAlg == -1，NSDMI 保证可读），转调品种
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 0), 6.0);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 1), 8.0);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 2), 10.0);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 7), 0.0);

	// 合约自己的按手数算法
	c->setFeeRates(1.0, 2.0, 3.0, true);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 4.0, 0), 4.0);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 4.0, 1), 8.0);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 4.0, 2), 12.0);

	// 合约自己的按金额算法：amount 用的是品种的放大倍数
	c->setFeeRates(0.0001, 0.0001, 0.0002, false);
	// 3500 * 2 * 10 = 70000 -> 7.0
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 0), 7.0);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 2), 14.0);
	c->release();
	comm->release();
}

TEST(l03_contract, calc_fee_keeps_cents_above_int32_range)
{
	WTSCommodityInfo* comm = mkComm(1);
	comm->setFeeRates(1.0, 1.0, 1.0, true);
	WTSContractInfo* contract = mkContract();
	contract->setCommInfo(comm);

	EXPECT_DOUBLE_EQ(21474836.46, comm->calcFee(1.0, 21474836.46, 0));
	EXPECT_DOUBLE_EQ(21474836.48, comm->calcFee(1.0, 21474836.48, 0));
	EXPECT_DOUBLE_EQ(30000000.0, comm->calcFee(1.0, 30000000.0, 0));
	EXPECT_DOUBLE_EQ(-1.22, comm->calcFee(1.0, -1.234, 0));

	contract->setFeeRates(1.0, 1.0, 1.0, true);
	EXPECT_DOUBLE_EQ(21474836.46, contract->calcFee(1.0, 21474836.46, 0));
	EXPECT_DOUBLE_EQ(21474836.48, contract->calcFee(1.0, 21474836.48, 0));
	EXPECT_DOUBLE_EQ(30000000.0, contract->calcFee(1.0, 30000000.0, 0));
	EXPECT_DOUBLE_EQ(-1.22, contract->calcFee(1.0, -1.234, 0));

	contract->release();
	comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 主力切换列表
//////////////////////////////////////////////////////////////////////////
TEST(l03_contract, hot_codes)
{
	WTSContractInfo* c = mkContract();
	EXPECT_TRUE(c->isFlat());
	c->addHotCode("rb2410");
	EXPECT_FALSE(c->isFlat());
	c->addHotCode("rb2501");
	c->addHotCode("rb2501");
	const std::vector<std::string>& codes = c->getHotCodes();
	ASSERT_EQ(codes.size(), 3u);
	EXPECT_EQ(codes[0], "rb2410");
	EXPECT_EQ(codes[2], "rb2501");
	// 追加型容器，不做去重
	EXPECT_EQ(c->getHotCodes().size(), 3u);
	c->release();
}

TEST(l03_contract, total_index_round_trip)
{
	WTSContractInfo* c = mkContract();
	c->setTotalIndex(0);
	EXPECT_EQ(c->getTotalIndex(), 0u);
	c->setTotalIndex(4096);
	EXPECT_EQ(c->getTotalIndex(), 4096u);
	c->setTotalIndex(std::numeric_limits<uint32_t>::max());
	EXPECT_EQ(c->getTotalIndex(), UINT_MAX);
	c->release();
}

//////////////////////////////////////////////////////////////////////////
// 扩展数据槽位
//////////////////////////////////////////////////////////////////////////
TEST(l03_contract, ext_data_slots)
{
	WTSContractInfo* c = mkContract();
	int a = 17;
	double b = 3.5;
	// 默认槽位 0
	c->setExtData<>(&a);
	EXPECT_EQ(c->getExtData<int>(), &a);
	// 显式模板索引（模板实参里的逗号需要用局部变量避开宏参数切分）
	c->setExtData<3>(&b);
	double* pSlot3 = c->getExtData<double, 3>();
	EXPECT_EQ(pSlot3, &b);
	// 其他槽位仍为空
	int* pSlot1 = c->getExtData<int, 1>();
	EXPECT_EQ(pSlot1, nullptr);
	// 覆盖写入
	c->setExtData<0>(nullptr);
	int* pSlot0 = c->getExtData<int, 0>();
	EXPECT_EQ(pSlot0, nullptr);
	c->release();
}

//////////////////////////////////////////////////////////////////////////
// 空指针风险：未挂接品种时直接读取保证金率/手续费会解引用 NULL
//////////////////////////////////////////////////////////////////////////
TEST(l03_contract, null_comm_info_is_guarded_by_caller_contract)
{
	WTSContractInfo* c = mkContract();
	ASSERT_EQ(c->getCommInfo(), nullptr);
	// getCommInfo/setCommInfo 自身是安全的
	c->setCommInfo(nullptr);
	EXPECT_EQ(c->getCommInfo(), nullptr);
	// 只有 flag == 1 的分支不触碰 m_commInfo
	c->setMarginRatios(0.09, 0.11, 1);
	EXPECT_DOUBLE_EQ(c->getLongMarginRatio(), 0.09);
	EXPECT_DOUBLE_EQ(c->getShortMarginRatio(), 0.11);
	c->release();
}

// 下面这条用例会在未挂接品种信息时解引用 NULL，属于源码缺陷（BUG-L03-005），
// 评审修复前保持 DISABLED，避免测试进程崩溃。
TEST(l03_contract, DISABLED_get_long_margin_ratio_without_comm_info)
{
	WTSContractInfo* c = mkContract();
	c->setMarginRatios(0.09, 0.11, 0);
	// 期望：返回合约自身保证金率；实际：解引用空 m_commInfo
	EXPECT_DOUBLE_EQ(c->getLongMarginRatio(), 0.09);
	c->release();
}
}  // namespace
