#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "Includes/WTSContractInfo.hpp"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WTSUtils/WtLMDB.hpp"

TEST(L04ContractImport, ValidStorageWidthIdentityPersistsAndReloads)
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto dir = std::filesystem::temp_directory_path() /
		("wt-l04-import-roundtrip-" + std::to_string(stamp));
	ASSERT_TRUE(std::filesystem::create_directory(dir));
	const auto file = dir / "contracts.json";
	const auto sessions = dir / "sessions.json";
	const std::string exchange(MAX_EXCHANGE_LENGTH - 1, 'E');
	const std::string code(MAX_INSTRUMENT_LENGTH - 1, 'C');
	// A rules-derived commodity must be ready before it is registered
	// (i03_commodity_ready_20261103 contract): positive pricetick, a volscale and
	// an already loaded session. Same data as l03_contract.storage_width_*.
	{
		std::ofstream output(sessions);
		ASSERT_TRUE(output.good());
		output << "{\"ALLDAY\":{\"name\":\"all day\",\"offset\":0,\"sections\":[{\"from\":0,\"to\":0}]}}";
		output.close();
		ASSERT_TRUE(output.good());
	}
	{
		std::ofstream output(file);
		ASSERT_TRUE(output.good());
		output << "{\"" << exchange << "\":{\"" << code
		       << "\":{\"name\":\"contract\",\"exchg\":\"" << exchange
		       << "\",\"rules\":{\"session\":\"ALLDAY\",\"pricetick\":1,\"volscale\":1}}}}";
		output.close();
		ASSERT_TRUE(output.good());
	}
	WTSBaseDataMgr manager;
	ASSERT_TRUE(manager.loadSessions(sessions.string().c_str()));
	ASSERT_TRUE(manager.loadContracts(file.string().c_str()));
	auto* contract = manager.getContract(code.c_str(), exchange.c_str());
	ASSERT_NE(nullptr, contract);
	ASSERT_STREQ(exchange.c_str(), contract->getExchg());
	ASSERT_STREQ(code.c_str(), contract->getCode());
	LMDBBarKey bar(contract->getExchg(), contract->getCode(), 20260101);
	LMDBHftKey tick(contract->getExchg(), contract->getCode(), 20260101, 93100000);
	const auto dbPath = (dir / "db").string();
	const std::string barKey(reinterpret_cast<const char*>(&bar), sizeof(bar));
	const std::string tickKey(reinterpret_cast<const char*>(&tick), sizeof(tick));
	{
		WtLMDB db;
		ASSERT_TRUE(db.open(dbPath.c_str()));
		{
			WtLMDBQuery query(db);
			ASSERT_TRUE(query.put_and_commit(barKey, "bar"));
		}
		{
			WtLMDBQuery query(db);
			ASSERT_TRUE(query.put_and_commit(tickKey, "tick"));
		}
	}
	{
		WtLMDB db(true);
		ASSERT_TRUE(db.open(dbPath.c_str()));
		WtLMDBQuery query(db);
		EXPECT_EQ("bar", query.get(barKey));
		EXPECT_EQ("tick", query.get(tickKey));
	}
	std::filesystem::remove_all(dir);
}
