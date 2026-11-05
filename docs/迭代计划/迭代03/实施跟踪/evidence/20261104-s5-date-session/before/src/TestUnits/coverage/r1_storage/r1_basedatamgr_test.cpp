// R1 / src/WTSTools/WTSBaseDataMgr.cpp — objects/exchgs 查询、loaders、交易日历边界
//
// 覆盖（全部公开入口，无线程、无共享内存）：
//  - loadSessions/loadCommodities/loadContracts/loadHolidays：缺文件、坏 json、缺 session 跳过、
//    rules 自动建品种、未知 product 跳过、altcode、opendate/expiredate、排序 setTotalIndex
//  - getContract/getContracts/getContractSize/getContractByIndex/getGlobalSize 缺失键行为
//  - getSession/getSessionByCode/getAllSessions/getSessionComms/getTplIDByPID
//  - isHoliday/isTradingDate/setTradingDate/getTradingDate/getNextTDate/getPrevTDate（确定性日期）
//  - getBoundaryTime：未知 pid→0、offset==0 周末前后移、offset>0（夜盘）分支、isSession 分支
//  - calcTradingDate：未知 pid 原样返回、offset==0 周末→下个交易日、offset>0 跨夜→下一交易日
//
// 日期基准：20240108=周一，20240106=周六，20240107=周日，20240113=周六，20240115=周一。
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/ILogHandler.h"
#include "Includes/FasterDefs.h"
#include "Share/StrUtil.hpp"
#include "Share/StdUtils.hpp"
#include "Share/TimeUtils.hpp"
#include "Share/fmtlib.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"

namespace r1 {

namespace {
std::string write_tmp(const char* name, const std::string& body)
{
	static uint32_t seq = 0;
	std::error_code ec;
	auto dir = std::filesystem::temp_directory_path() /
		fmtutil::format("r1_bdm_{0}_{1}", (uint32_t)::getpid(), ++seq);
	std::filesystem::create_directories(dir, ec);
	auto file = dir / name;
	std::ofstream ofs(file.string());
	ofs << body;
	ofs.close();
	return file.string();
}

std::string json_quote(const std::string& value)
{
	std::string result = "\"";
	for (char ch : value)
	{
		if (ch == '\\' || ch == '"')
			result += '\\';
		result += ch;
	}
	result += '"';
	return result;
}

struct CommodityIdentityConfig
{
	std::string exchg = "SHFE";
	std::string pid = "rb";
	std::string name = "rebar";
	std::string session = "DAY";
	std::string holiday = "CHINA";
};

std::string commodity_config(const CommodityIdentityConfig& fields)
{
	return "{" + json_quote(fields.exchg) + ":{" + json_quote(fields.pid) + ":{" +
		"\"name\":" + json_quote(fields.name) + "," +
		"\"session\":" + json_quote(fields.session) + "," +
		"\"holiday\":" + json_quote(fields.holiday) + "}}}";
}

std::string session_config(const std::string& session)
{
	return "{" + json_quote(session) + ":{\"name\":\"boundary session\",\"offset\":0," +
		"\"sections\":[{\"from\":900,\"to\":1500}]}}";
}

struct ContractIdentityConfig
{
	std::string code = "rb2401";
	std::string name = "rebar 2401";
	std::string exchg = "SHFE";
	std::string product = "rb";
	std::string altCode = "ALT";
};

std::string contract_config(const ContractIdentityConfig& fields)
{
	return "{\"OUTER\":{" + json_quote(fields.code) + ":{" +
		"\"name\":" + json_quote(fields.name) + "," +
		"\"exchg\":" + json_quote(fields.exchg) + "," +
		"\"product\":" + json_quote(fields.product) + "," +
		"\"altcode\":" + json_quote(fields.altCode) + "}}}";
}

class ScopedBoundaryLogCapture : public ILogHandler
{
public:
	ScopedBoundaryLogCapture()
	{
		static const bool loggerInitialized = []() {
			WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})", false, nullptr);
			return true;
		}();
		(void)loggerInitialized;
		WTSLogger::registerHandler(this);
	}

	~ScopedBoundaryLogCapture()
	{
		WTSLogger::registerHandler(nullptr);
	}

	void handleLogAppend(WTSLogLevel, const char* message) override
	{
		if (message != nullptr)
			messages.emplace_back(message);
	}

	void clear() { messages.clear(); }
	bool contains(const char* fragment) const
	{
		return std::any_of(messages.begin(), messages.end(), [fragment](const std::string& message) {
			return message.find(fragment) != std::string::npos;
		});
	}

	std::vector<std::string> messages;
};
} // namespace

class R1BaseDataMgrTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sess_file = write_tmp("sessions.json", R1_SESSIONS_JSON);
		_comm_file = write_tmp("commodities.json", R1_COMMODITIES_JSON);
		_ct_file = write_tmp("contracts.json", R1_CONTRACTS_JSON);
		_hol_file = write_tmp("holidays.json", R1_HOLIDAYS_JSON);
		ASSERT_TRUE(_mgr.loadSessions(_sess_file.c_str()));
		ASSERT_TRUE(_mgr.loadCommodities(_comm_file.c_str()));
		ASSERT_TRUE(_mgr.loadContracts(_ct_file.c_str()));
		ASSERT_TRUE(_mgr.loadHolidays(_hol_file.c_str()));
	}

	static constexpr const char* R1_SESSIONS_JSON = R"({
	"DAY0":{"name":"日盘","offset":0,"auction":{"from":915,"to":925},
		"sections":[{"from":900,"to":1130},{"from":1300,"to":1500}]},
	"DAYA":{"name":"多集合竞价","offset":0,
		"auctions":[{"from":915,"to":925},{"from":1457,"to":1500}],
		"sections":[{"from":930,"to":1130}]},
	"NIGHT":{"name":"夜盘偏移","offset":300,
		"sections":[{"from":2100,"to":2300},{"from":900,"to":1500}]},
	"OUTER":{"name":"外盘","offset":-300,
		"sections":[{"from":2100,"to":2359}]},
	"BADSEC":{"name":"无sections","offset":0}
})";

	static constexpr const char* R1_COMMODITIES_JSON = R"({
	"SSE":{"STK":{"name":"股票","session":"DAY0","holiday":"CHINA","pricetick":0.01,
		"volscale":1,"category":0,"covermode":0,"pricemode":0,"trademode":1,"lotstick":1,"minlots":1}},
	"SHFE":{"rb":{"name":"螺纹钢","session":"NIGHT","holiday":"CHINA","pricetick":1,
		"volscale":10,"covermode":0,"pricemode":1},
		"nosec":{"name":"缺session","pricetick":1,"volscale":1,"covermode":0,"pricemode":0},
		"ghost":{"name":"假session","session":"NOPE","pricetick":1,"volscale":1,"covermode":0,"pricemode":0}}
})";

	static constexpr const char* R1_CONTRACTS_JSON = R"({
	"SSE":{
		"600000":{"name":"浦发银行","exchg":"SSE","product":"STK","altcode":"SH600000",
			"opendate":20200101,"expiredate":20500101,"maxlimitqty":100000,"minlimitqty":1},
		"600999":{"name":"退市股","exchg":"SSE","product":"STK",
			"opendate":20100101,"expiredate":20231231,"volscale":5},
		"BADPROD":{"name":"未知品种","exchg":"SSE","product":"NOPE"}
	},
	"SHFE":{
		"rb2401":{"name":"螺纹2401","exchg":"SHFE","product":"rb",
			"opendate":20230110,"expiredate":20240115,"longmarginratio":0.1,"shortmarginratio":0.12},
		"autoX":{"name":"自动品种","exchg":"SHFE",
			"rules":{"session":"NIGHT","holiday":"CHINA","pricetick":2,"volscale":3,
				"covermode":1,"pricemode":0}}
	}
})";

	static constexpr const char* R1_HOLIDAYS_JSON = R"({"CHINA":[20240101,20240210]})";

	WTSBaseDataMgr	_mgr;
	std::string	_sess_file;
	std::string	_comm_file;
	std::string	_ct_file;
	std::string	_hol_file;
};

TEST_F(R1BaseDataMgrTest, loaders_reject_missing_and_broken_files)
{
	WTSBaseDataMgr fresh;
	EXPECT_FALSE(fresh.loadSessions("nope_not_exists.json"));
	EXPECT_FALSE(fresh.loadCommodities("nope_not_exists.json"));
	EXPECT_FALSE(fresh.loadContracts("nope_not_exists.json"));
	EXPECT_FALSE(fresh.loadHolidays("nope_not_exists.json"));

	std::string broken = write_tmp("broken.json", "this is not json {{{");
	WTSBaseDataMgr fresh2;
	EXPECT_FALSE(fresh2.loadSessions(broken.c_str()));
	EXPECT_FALSE(fresh2.loadCommodities(broken.c_str()));
	EXPECT_FALSE(fresh2.loadContracts(broken.c_str()));
	EXPECT_FALSE(fresh2.loadHolidays(broken.c_str()));
}

TEST(R1BaseDataMgrOwnership, created_map_values_are_released_with_manager)
{
	const std::string sessions = write_tmp("ownership_sessions.json", R"({
		"DAY":{"name":"day","offset":0,"sections":[{"from":900,"to":1500}]}
	})");
	const std::string commodities = write_tmp("ownership_commodities.json", R"({
		"SHFE":{"rb":{"name":"rebar","session":"DAY","pricetick":1,"volscale":10}}
	})");
	const std::string contracts = write_tmp("ownership_contracts.json", R"({
		"SHFE":{"rb2401":{"name":"rebar 2401","exchg":"SHFE","product":"rb"}}
	})");

	{
		WTSBaseDataMgr manager;
		ASSERT_TRUE(manager.loadSessions(sessions.c_str()));
		ASSERT_TRUE(manager.loadCommodities(commodities.c_str()));
		ASSERT_TRUE(manager.loadContracts(contracts.c_str()));
		ASSERT_NE(manager.getSession("DAY"), nullptr);
		ASSERT_NE(manager.getCommodity("SHFE.rb"), nullptr);
		ASSERT_NE(manager.getContract("rb2401", "SHFE"), nullptr);
	}
}

TEST(R1BaseDataMgrOwnership, commodity_session_assignment_owns_references)
{
	WTSCommodityInfo* commodity = WTSCommodityInfo::create("rb", "rebar", "SHFE", "DAY", "CHINA");
	ASSERT_NE(commodity, nullptr);
	EXPECT_EQ(commodity->getSessionInfo(), nullptr);
	commodity->setSessionInfo(nullptr);
	EXPECT_EQ(commodity->getSessionInfo(), nullptr);

	WTSSessionInfo* first = WTSSessionInfo::create("DAY", "first", 0);
	WTSSessionInfo* second = WTSSessionInfo::create("DAY", "second", 300);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(first->retainCount(), 1u);
	EXPECT_EQ(second->retainCount(), 1u);

	commodity->setSessionInfo(first);
	EXPECT_EQ(first->retainCount(), 2u);
	commodity->setSessionInfo(first);
	EXPECT_EQ(first->retainCount(), 2u);

	commodity->setSessionInfo(second);
	EXPECT_EQ(first->retainCount(), 1u);
	EXPECT_EQ(second->retainCount(), 2u);
	commodity->setSessionInfo(nullptr);
	EXPECT_EQ(second->retainCount(), 1u);
	EXPECT_EQ(commodity->getSessionInfo(), nullptr);

	first->release();
	second->release();
	commodity->release();
}

TEST(R1BaseDataMgrOwnership, reload_and_external_commodity_keep_sessions_alive)
{
	const std::string firstSessions = write_tmp("reload_sessions_first.json", R"({
		"DAY":{"name":"first","offset":0,"sections":[{"from":900,"to":1500}]}
	})");
	const std::string secondSessions = write_tmp("reload_sessions_second.json", R"({
		"DAY":{"name":"second","offset":300,"sections":[{"from":2100,"to":2300}]}
	})");
	const std::string commodities = write_tmp("reload_commodities.json", R"({
		"SHFE":{"rb":{"name":"rebar","session":"DAY","pricetick":1,"volscale":10}}
	})");
	struct HeldCommodity
	{
		WTSCommodityInfo* value = nullptr;
		~HeldCommodity() { if (value) value->release(); }
	} held;
	WTSSessionInfo* firstSession = nullptr;
	{
		WTSBaseDataMgr manager;
		ASSERT_TRUE(manager.loadSessions(firstSessions.c_str()));
		ASSERT_TRUE(manager.loadCommodities(commodities.c_str()));
		held.value = manager.getCommodity("SHFE.rb");
		ASSERT_NE(held.value, nullptr);
		held.value->retain();
		firstSession = held.value->getSessionInfo();
		ASSERT_NE(firstSession, nullptr);
		EXPECT_EQ(firstSession->getOffsetMins(), 0);
		EXPECT_EQ(firstSession->retainCount(), 2u); // manager map + commodity

		ASSERT_TRUE(manager.loadSessions(secondSessions.c_str()));
		EXPECT_EQ(manager.getSession("DAY")->getOffsetMins(), 300);
		EXPECT_EQ(held.value->getSessionInfo(), firstSession);
		EXPECT_EQ(firstSession->getOffsetMins(), 0);
		EXPECT_EQ(firstSession->retainCount(), 1u); // commodity after map replacement
	}

	ASSERT_NE(held.value, nullptr);
	ASSERT_NE(held.value->getSessionInfo(), nullptr);
	EXPECT_EQ(held.value->getSessionInfo(), firstSession);
	EXPECT_EQ(held.value->getSessionInfo()->getOffsetMins(), 0);
}

TEST(R1BaseDataMgrConfigBoundary, commodity_identity_fields_enforce_caps_and_retain_values)
{
	ScopedBoundaryLogCapture logs;
	const std::array<std::size_t, 3> acceptedCaps = { 63, 64, 65 };
	const std::array<const char*, 5> fieldNames = { "exchg", "pid", "name", "session", "holiday" };

	for (std::size_t field = 0; field < fieldNames.size(); ++field)
	{
		for (std::size_t edge = 0; edge < 4; ++edge)
		{
			CommodityIdentityConfig fields;
			const std::string value = edge == 3
				? std::string()
				: std::string(acceptedCaps[edge], static_cast<char>('a' + field));
			switch (field)
			{
			case 0: fields.exchg = value; break;
			case 1: fields.pid = value; break;
			case 2: fields.name = value; break;
			case 3: fields.session = value; break;
			case 4: fields.holiday = value; break;
			}

			const std::string sessionId = fields.session.empty() ? "DAY" : fields.session;
			const std::string sessionsPath = write_tmp("identity_sessions.json", session_config(sessionId));
			const std::string commoditiesPath = write_tmp("identity_commodities.json", commodity_config(fields));
			WTSBaseDataMgr manager;
			ASSERT_TRUE(manager.loadSessions(sessionsPath.c_str())) << fieldNames[field] << " edge " << edge;
			logs.clear();
			const bool loaded = manager.loadCommodities(commoditiesPath.c_str());

			const bool oversized = edge == 1 || edge == 2;
			if (oversized)
			{
				EXPECT_FALSE(loaded) << fieldNames[field] << " length " << value.size();
				EXPECT_TRUE(logs.contains("Commodity identity exceeds field capacity"))
					<< fieldNames[field] << " length " << value.size();
				EXPECT_EQ(nullptr, manager.getSessionComms(sessionId.c_str()));
				continue;
			}

			if (field == 3 && edge == 3)
			{
				EXPECT_TRUE(loaded);
				EXPECT_TRUE(logs.contains("No session configured"));
				EXPECT_EQ(nullptr, manager.getSessionComms(""));
				continue;
			}

			EXPECT_TRUE(loaded) << fieldNames[field] << " edge " << edge;
			CodeSet* sessionComms = manager.getSessionComms(sessionId.c_str());
			ASSERT_NE(nullptr, sessionComms) << fieldNames[field] << " edge " << edge;
			ASSERT_EQ(1u, sessionComms->size()) << fieldNames[field] << " edge " << edge;
			const std::string fullPid = *sessionComms->begin();
			WTSCommodityInfo* registered = manager.getCommodity(fullPid.c_str());
			ASSERT_NE(nullptr, registered) << fieldNames[field] << " edge " << edge;
			EXPECT_EQ(fields.pid, registered->getProduct());
			EXPECT_EQ(fields.name, registered->getName());
			EXPECT_EQ(fields.exchg, registered->getExchg());
			EXPECT_EQ(fields.session, registered->getSession());
			EXPECT_EQ(fields.holiday, registered->getTradingTpl());
			EXPECT_STREQ("CNY", registered->getCurrency());
		}
	}
}

TEST(R1BaseDataMgrConfigBoundary, contract_identity_fields_enforce_caps_and_retain_values)
{
	ScopedBoundaryLogCapture logs;
	// Import must also fit AD's narrower storage key, independently of the
	// generic WTSContractInfo object's larger exchange field.
	const std::array<std::size_t, 4> capacities = { 32, 64, MAX_EXCHANGE_LENGTH, 32 };
	const std::array<const char*, 4> fieldNames = { "code", "name", "exchg", "product" };

	for (std::size_t field = 0; field < fieldNames.size(); ++field)
	{
		for (std::size_t edge = 0; edge < 4; ++edge)
		{
			ContractIdentityConfig fields;
			const std::string value = edge == 3
				? std::string()
				: std::string(capacities[field] - 1 + edge, static_cast<char>('k' + field));
			switch (field)
			{
			case 0: fields.code = value; break;
			case 1: fields.name = value; break;
			case 2: fields.exchg = value; break;
			case 3: fields.product = value; break;
			}

			const std::string sessionsPath = write_tmp("contract_identity_sessions.json", session_config("DAY"));
			CommodityIdentityConfig commodity;
			commodity.exchg = fields.exchg;
			commodity.pid = fields.product;
			const std::string commoditiesPath = write_tmp("contract_identity_commodities.json", commodity_config(commodity));
			const std::string contractsPath = write_tmp("identity_contracts.json", contract_config(fields));
			WTSBaseDataMgr manager;
			ASSERT_TRUE(manager.loadSessions(sessionsPath.c_str()));
			ASSERT_TRUE(manager.loadCommodities(commoditiesPath.c_str())) << fieldNames[field] << " edge " << edge;
			logs.clear();
			const bool loaded = manager.loadContracts(contractsPath.c_str());

			const bool oversized = edge == 1 || edge == 2;
			if (oversized)
			{
				EXPECT_FALSE(loaded) << fieldNames[field] << " length " << value.size();
				const char* expected = (field == 0 || field == 2)
					? "Contract identity exceeds storage field capacity"
					: "Contract identity exceeds field capacity";
				EXPECT_TRUE(logs.contains(expected))
					<< fieldNames[field] << " length " << value.size();
				EXPECT_EQ(0u, manager.getGlobalSize());
				continue;
			}

			EXPECT_TRUE(loaded) << fieldNames[field] << " edge " << edge;
			ASSERT_EQ(1u, manager.getGlobalSize()) << fieldNames[field] << " edge " << edge;
			WTSContractInfo* registered = manager.getContract(fields.code.c_str(), fields.exchg.c_str());
			ASSERT_NE(nullptr, registered) << fieldNames[field] << " edge " << edge;
			EXPECT_EQ(fields.code, registered->getCode());
			EXPECT_EQ(fields.name, registered->getName());
			EXPECT_EQ(fields.exchg, registered->getExchg());
			EXPECT_EQ(fields.product, registered->getProduct());
			EXPECT_STREQ("ALT", registered->getAltCode());
		}
	}
}

TEST(R1BaseDataMgrConfigBoundary, configured_altcode_caps_and_empty_fallback_are_observable)
{
	ScopedBoundaryLogCapture logs;
	const std::array<std::size_t, 3> altCodeLengths = { 31, 32, 33 };
	for (std::size_t edge = 0; edge < 4; ++edge)
	{
		ContractIdentityConfig fields;
		fields.altCode = edge == 3 ? std::string() : std::string(altCodeLengths[edge], 'z');
		const std::string sessionsPath = write_tmp("altcode_sessions.json", session_config("DAY"));
		CommodityIdentityConfig commodity;
		commodity.exchg = fields.exchg;
		commodity.pid = fields.product;
		const std::string commoditiesPath = write_tmp("altcode_commodities.json", commodity_config(commodity));
		const std::string contractsPath = write_tmp("altcode_contracts.json", contract_config(fields));
		WTSBaseDataMgr manager;
		ASSERT_TRUE(manager.loadSessions(sessionsPath.c_str()));
		ASSERT_TRUE(manager.loadCommodities(commoditiesPath.c_str()));
		logs.clear();
		const bool loaded = manager.loadContracts(contractsPath.c_str());

		if (edge == 1 || edge == 2)
		{
			EXPECT_FALSE(loaded) << "altcode length " << fields.altCode.size();
			EXPECT_TRUE(logs.contains("Contract identity exceeds field capacity"));
			EXPECT_EQ(0u, manager.getGlobalSize());
			continue;
		}

		EXPECT_TRUE(loaded);
		ASSERT_EQ(1u, manager.getGlobalSize());
		WTSContractInfo* registered = manager.getContract(fields.code.c_str(), fields.exchg.c_str());
		ASSERT_NE(nullptr, registered);
		const std::string expectedAltCode = fields.altCode.empty() ? fields.code : fields.altCode;
		EXPECT_EQ(expectedAltCode, registered->getAltCode());
		EXPECT_EQ(registered, manager.getContract(expectedAltCode.c_str(), fields.exchg.c_str()));
	}
}


TEST_F(R1BaseDataMgrTest, sessions_loaded_and_missing_sections_skipped)
{
	WTSSessionInfo* day0 = _mgr.getSession("DAY0");
	ASSERT_TRUE(day0 != NULL);
	EXPECT_EQ(day0->getOffsetMins(), 0);
	EXPECT_EQ(day0->getOpenTime(), 900u);
	EXPECT_EQ(day0->getCloseTime(), 1500u);
	WTSSessionInfo* daya = _mgr.getSession("DAYA");
	ASSERT_TRUE(daya != NULL);
	// BADSEC 没有 sections：loadSessions 在加入 session map 前跳过该项。
	EXPECT_EQ(_mgr.getSession("BADSEC"), nullptr);
	WTSArray* all = _mgr.getAllSessions();
	ASSERT_TRUE(all != NULL);
	// DAY0/DAYA/NIGHT/OUTER 共 4 个。
	std::size_t expect_cnt = 4;
	EXPECT_EQ(all->size(), expect_cnt);
	all->release();
}

TEST_F(R1BaseDataMgrTest, commodities_lookup_and_missing_session_branches)
{
	WTSCommodityInfo* stk = _mgr.getCommodity("SSE.STK");
	ASSERT_TRUE(stk != NULL);
	EXPECT_TRUE(stk->isStock());			// category 0 = CC_Stock
	EXPECT_DOUBLE_EQ(stk->getPriceTick(), 0.01);
	EXPECT_EQ(stk->getVolScale(), 1u);

	WTSCommodityInfo* rb = _mgr.getCommodity("SHFE", "rb");
	ASSERT_TRUE(rb != NULL);
	EXPECT_TRUE(rb->isFuture());
	EXPECT_EQ(rb->getVolScale(), 10u);

	// 缺 session → warn+continue；session 不存在 → error+continue：两者都不入库
	EXPECT_EQ(_mgr.getCommodity("SHFE.nosec"), nullptr);
	EXPECT_EQ(_mgr.getCommodity("SHFE", "nosec"), nullptr);
	EXPECT_EQ(_mgr.getCommodity("SHFE.ghost"), nullptr);
	EXPECT_EQ(_mgr.getCommodity("NOPE.NOPE"), nullptr);

	// getSessionComms：DAY0 挂了 SSE.STK，NIGHT 挂了 SHFE.rb 与 autoX
	CodeSet* dayComms = _mgr.getSessionComms("DAY0");
	ASSERT_TRUE(dayComms != NULL);
	EXPECT_NE(dayComms->find("SSE.STK"), dayComms->end());
	EXPECT_EQ(_mgr.getSessionComms("NOPE"), nullptr);
	// getTplIDByPID 是 private：经公开 isHoliday(pid,...,isTpl=false) 间接验证映射到 CHINA
	EXPECT_TRUE(_mgr.isHoliday("SSE.STK", 20240101, false));
	EXPECT_FALSE(_mgr.isHoliday("SSE.STK", 20240108, false));
}

TEST_F(R1BaseDataMgrTest, contract_lookup_by_code_fullcode_altcode_and_dates)
{
	WTSContractInfo* c = _mgr.getContract("600000", "");
	ASSERT_TRUE(c != NULL);
	EXPECT_STREQ(c->getCode(), "600000");
	EXPECT_STREQ(c->getExchg(), "SSE");
	// altcode 也进 _code_map
	WTSContractInfo* alt = _mgr.getContract("SH600000", "");
	ASSERT_TRUE(alt != NULL);
	EXPECT_STREQ(alt->getCode(), "600000");
	// 全码查询
	ASSERT_TRUE(_mgr.getContract("600000", "SSE") != NULL);
	// 错误市场 → 缺失键
	EXPECT_EQ(_mgr.getContract("600000", "SHFE"), nullptr);
	EXPECT_EQ(_mgr.getContract("nosuch", ""), nullptr);
	// 有效期窗口：退市股 20231231 到期
	WTSContractInfo* delisted = _mgr.getContract("600999", "SSE");
	ASSERT_TRUE(delisted != NULL);
	EXPECT_EQ(delisted->getVolScale(), 5u);		// 合约级 volscale 覆盖品种级
	EXPECT_EQ(_mgr.getContract("600999", "SSE", 20240101), nullptr);
	ASSERT_TRUE(_mgr.getContract("600999", "SSE", 20200601) != NULL);
	// 未知 product 且无 rules 的合约被跳过
	EXPECT_EQ(_mgr.getContract("BADPROD", ""), nullptr);
	// rules 自动生成品种
	WTSCommodityInfo* autoComm = _mgr.getCommodity("SHFE", "autoX");
	ASSERT_TRUE(autoComm != NULL);
	EXPECT_DOUBLE_EQ(autoComm->getPriceTick(), 2.0);
}

TEST_F(R1BaseDataMgrTest, code_list_sorted_by_fullcode_and_index_queries)
{
	// 600000/600999/rb2401/autoX 共 4 条（BADPROD 被跳过）
	EXPECT_EQ(_mgr.getGlobalSize(), 4u);
	EXPECT_EQ(_mgr.getContractSize(""), 4u);
	EXPECT_EQ(_mgr.getContractSize("SSE"), 2u);
	// 按日期过滤：20240101 时 600999 已退市；autoX 无 expiredate(=0) 也过不了窗口
	EXPECT_EQ(_mgr.getContractSize("", 20240101), 2u);

	// strcmp 排序：SHFE.autoX < SHFE.rb2401 < SSE.600000 < SSE.600999
	WTSContractInfo* first = _mgr.getContractByIndex(0);
	ASSERT_TRUE(first != NULL);
	EXPECT_STREQ(first->getFullCode(), "SHFE.autoX");
	EXPECT_EQ(first->getTotalIndex(), 0u);
	WTSContractInfo* last = _mgr.getContractByIndex(3);
	ASSERT_TRUE(last != NULL);
	EXPECT_STREQ(last->getFullCode(), "SSE.600999");
	EXPECT_EQ(_mgr.getContractByIndex(4), nullptr);

	WTSArray* ay = _mgr.getContracts("SHFE");
	ASSERT_TRUE(ay != NULL);
	EXPECT_EQ(ay->size(), (std::size_t)2);
	ay->release();
	// getSessionByCode 走合约→品种→时间链
	WTSSessionInfo* s = _mgr.getSessionByCode("600000", "SSE");
	ASSERT_TRUE(s != NULL);
	EXPECT_EQ(s->getCloseTime(), 1500u);
	EXPECT_EQ(_mgr.getSessionByCode("nosuch", "SSE"), nullptr);
}

TEST_F(R1BaseDataMgrTest, holidays_and_trading_date_arithmetic)
{
	EXPECT_TRUE(_mgr.isHoliday("CHINA", 20240101, true));	// 配置节假日
	EXPECT_TRUE(_mgr.isHoliday("CHINA", 20240106, true));	// 周六
	EXPECT_TRUE(_mgr.isHoliday("CHINA", 20240107, true));	// 周日
	EXPECT_FALSE(_mgr.isHoliday("CHINA", 20240108, true));	// 周一
	EXPECT_TRUE(_mgr.isHoliday("CHINA", 20240211, true));	// 周日
	EXPECT_FALSE(_mgr.isHoliday("CHINA", 20240212, true));	// 节后周一，非节假日

	// 未配置节假日的模板：非周末一律 false
	EXPECT_FALSE(_mgr.isHoliday("UNKNOWN", 20240109, true));

	EXPECT_TRUE(_mgr.isTradingDate("CHINA", 20240108, true));
	EXPECT_FALSE(_mgr.isTradingDate("CHINA", 20240106, true));

	// 跨节假日 +2 个交易日
	EXPECT_EQ(_mgr.getNextTDate("CHINA", 20240208, 1, true), 20240209u);	// 周四→周五
	EXPECT_EQ(_mgr.getNextTDate("CHINA", 20240209, 1, true), 20240212u);	// 周五→(周六日+20240210节假日)→周一
	EXPECT_EQ(_mgr.getNextTDate("CHINA", 20240209, 2, true), 20240213u);
	EXPECT_EQ(_mgr.getPrevTDate("CHINA", 20240108, 1, true), 20240105u);	// 周一→上周五
	EXPECT_EQ(_mgr.getPrevTDate("CHINA", 20240213, 1, true), 20240212u); // 周二→按 fixture 为工作日的周一

	// 品种 pid 通过 getTplIDByPID 映射到 CHINA
	EXPECT_EQ(_mgr.getNextTDate("SSE.STK", 20240112, 1), 20240115u);		// 周五→周一

	// set/getTradingDate 缓存 cur_tdate
	_mgr.setTradingDate("CHINA", 20240301, true);
	EXPECT_EQ(_mgr.getTradingDate("CHINA", 0, 0, true), 20240301u);
	// 未配置模板 → 返回当前日期（>20200101 即可，不做墙钟等值断言）
	EXPECT_GT(_mgr.getTradingDate("NOPE", 0, 0, true), 20200101u);
	// 周六偏移日 → 顺延下个交易日（不清缓存路径用 uOffDate）
	_mgr.setTradingDate("CHINA", 0, true);		// 清缓存语义：0 视为未设置
	EXPECT_EQ(_mgr.getTradingDate("CHINA", 20240113, 0, true), 20240115u);
}

TEST_F(R1BaseDataMgrTest, invalid_dates_fail_closed_for_trading_calendar_queries)
{
	// isHoliday has no error channel, so an invalid date is conservatively
	// considered unavailable; isTradingDate must never call it tradable.
	EXPECT_EQ(TimeUtils::getWeekDay(20240230u), TimeUtils::INVALID_WEEKDAY);
	EXPECT_TRUE(_mgr.isHoliday("CHINA", 20240230u, true));
	EXPECT_FALSE(_mgr.isTradingDate("CHINA", 20240230u, true));
	EXPECT_TRUE(_mgr.isHoliday("CHINA", std::numeric_limits<uint32_t>::max(), true));
	EXPECT_FALSE(_mgr.isTradingDate("CHINA", std::numeric_limits<uint32_t>::max(), true));
}

TEST_F(R1BaseDataMgrTest, boundary_time_offsets_and_weekend_shift)
{
	// 未知 pid → getCommodity NULL → 0
	EXPECT_EQ(_mgr.getBoundaryTime("NOPE.NOPE", 20240110, false, true), 0ull);

	// offset==0（DAY0/SSE.STK）：工作日直接返回开收盘
	EXPECT_EQ(_mgr.getBoundaryTime("SSE.STK", 20240110, false, true), 202401100900ull);
	EXPECT_EQ(_mgr.getBoundaryTime("SSE.STK", 20240110, false, false), 202401101500ull);
	// 周六 start → 先顺延到周一 20240115
	EXPECT_EQ(_mgr.getBoundaryTime("SSE.STK", 20240113, false, true), 202401150900ull);
	// 周六 end → 回退到周五 20240112
	EXPECT_EQ(_mgr.getBoundaryTime("SSE.STK", 20240113, false, false), 202401121500ull);

	// offset>0（NIGHT/SHFE.rb，first section 2100）：
	// isStart → 上一交易日 20240109 的 21:00；isEnd → 当日收盘 1500
	EXPECT_EQ(_mgr.getBoundaryTime("SHFE.rb", 20240110, false, true), 202401092100ull);
	EXPECT_EQ(_mgr.getBoundaryTime("SHFE.rb", 20240110, false, false), 202401101500ull);

	// offset<0（OUTER 无挂品种，走 isSession==true 分支：getSession("OUTER") + CHINA 模板）
	// getOffsetMins()==-300 <0：isStart → tDate*10000+open(2100)；isEnd → 次日*10000+close(2359)
	EXPECT_EQ(_mgr.getBoundaryTime("OUTER", 20240110, true, true), 202401102100ull);
	EXPECT_EQ(_mgr.getBoundaryTime("OUTER", 20240110, true, false), 202401112359ull);
	// 缺少 sections 的 session 在加载时被跳过；未知 session 的边界查询返回 0。
	EXPECT_EQ(_mgr.getBoundaryTime("BADSEC", 20240110, true, true), 0ull);
}

TEST_F(R1BaseDataMgrTest, calc_trading_date_branches)
{
	// 未知 pid → getCommodity NULL → 原样返回 uDate
	EXPECT_EQ(_mgr.calcTradingDate("NOPE.NOPE", 20240110, 1000, false), 20240110u);

	// offset==0 工作日 → 自然日
	EXPECT_EQ(_mgr.calcTradingDate("SSE.STK", 20240110, 1000, false), 20240110u);
	// offset==0 周六 → 下一交易日
	EXPECT_EQ(_mgr.calcTradingDate("SSE.STK", 20240113, 1000, false), 20240115u);

	// offset>0（NIGHT，偏移 300 分钟 → offMin=0500）：
	// 23:00 > 05:00 → 跨夜，交易日=下一交易日 20240111
	EXPECT_EQ(_mgr.calcTradingDate("SHFE.rb", 20240110, 2300, false), 20240111u);
	// 04:00 < 05:00 且周二 → 不跨日，交易日=自然日
	EXPECT_EQ(_mgr.calcTradingDate("SHFE.rb", 20240110, 400, false), 20240110u);

	// offset<0（OUTER）走 isSession 模板分支：getSession 命中
	WTSSessionInfo* s = _mgr.getSession("OUTER");
	ASSERT_TRUE(s != NULL);
	EXPECT_EQ(s->getOffsetMins(), -300);

	// isSession==true 且 session 不存在 → sInfo NULL → 原样返回
	EXPECT_EQ(_mgr.calcTradingDate("NOPE", 20240110, 1000, true), 20240110u);
}

TEST_F(R1BaseDataMgrTest, missing_commodity_pid_falls_back_to_empty_tpl)
{
	// SHFE.nosec 未入库 → getTplIDByPID 返回 "" → 模板查不到 → 工作日 false
	EXPECT_FALSE(_mgr.isHoliday("SHFE.nosec", 20240108, false));
	EXPECT_FALSE(_mgr.isHoliday("SHFE.nosec", 20240101, false));
	// 已入库品种走 SHFE.rb → CHINA 模板
	EXPECT_TRUE(_mgr.isHoliday("SHFE.rb", 20240101, false));
}

TEST_F(R1BaseDataMgrTest, getTplIDByPID_malformed_and_valid_pid_holiday_semantics)
{
	// 非法/空 PID 在工作日查不到模板，保持 false。
	EXPECT_FALSE(_mgr.isHoliday("NOPIVOT", 20240108, false));
	EXPECT_FALSE(_mgr.isHoliday("", 20240108, false));

	// 周末检查先于 PID 映射；非法/空 PID 仍按周末返回 true。
	EXPECT_TRUE(_mgr.isHoliday("NOPIVOT", 20240106, false));
	EXPECT_TRUE(_mgr.isHoliday("", 20240107, false));

	// 已知品种仍映射到 CHINA 模板：配置节假日、普通工作日及周末语义不变。
	EXPECT_TRUE(_mgr.isHoliday("SSE.STK", 20240101, false));
	EXPECT_FALSE(_mgr.isHoliday("SSE.STK", 20240108, false));
	EXPECT_TRUE(_mgr.isHoliday("SSE.STK", 20240106, false));
}

} // namespace r1
