/*
 * lane R76 — ParserShm 公开面补测（边界/生命周期/迟到基础数据）
 *
 * 目标文件：src/ParserShm/ParserShm.cpp（385 可执行行，missed 308；模块含头文件 386/309）
 *
 * 既有 t5_shm_parser_test.cpp 已覆盖 init 正/负配置、disconnect/isConnected、
 * registerSpi 首次与替换、subscribe/unsubscribe 的标准码拆分、队列布局与工厂。
 * 本文件只补它没走到的组合：
 *   - registerSpi(NULL) 后再注册：bReplaced 必须回到 false，不能再报「Listener is replaced」；
 *   - 「注册时 getGlobalSize()==0、订阅前才变成 8」的迟到基础数据：命中
 *     ParserShm.cpp:286-287 的 `if (_subbed.empty()) resize(...)` 分支（既有用例都在
 *     registerSpi 时已把位图撑到 8，永远走不到这里）；
 *   - 注销 sink 后 subscribe/unsubscribe 仍复用旧 _bd_mgr 的现状；
 *   - disconnect 之后 subscribe 仍会查合约（无 _stopped 守卫）。
 *
 * 不建真实共享内存段、不起线程、不调 connect()：接收状态机 ParserShm.cpp:92-269
 * 仍按 lane 约定留白（见 bugs/R76.md 与 T5 清单里的 DISABLED_receive_path_needs_a_real_mapped_file）。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ParserShm/ParserShm.h"

#include "Includes/FasterDefs.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

extern "C"
{
	IParserApi* createParser();
	void deleteParser(IParserApi*& parser);
}

namespace r76shm
{

/* ------------------------------------------------------------------ 替身 */

class R76ShmBdMgr : public IBaseDataMgr
{
public:
	void setContract(WTSContractInfo* ct) { _ct = ct; }
	void setGlobalSize(uint32_t size) { _global_size = size; }

	virtual WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	virtual WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }

	virtual WTSContractInfo* getContract(const char* code, const char* exchg, uint32_t) override
	{
		++contract_calls;
		if (_ct == nullptr || code == nullptr || exchg == nullptr)
			return nullptr;
		if (strcmp(code, _ct->getCode()) == 0 && strcmp(exchg, _ct->getExchg()) == 0)
			return _ct;
		return nullptr;
	}

	virtual WTSArray* getContracts(const char*, uint32_t) override { return nullptr; }
	virtual WTSSessionInfo* getSession(const char*) override { return nullptr; }
	virtual WTSSessionInfo* getSessionByCode(const char*, const char*) override { return nullptr; }
	virtual WTSArray* getAllSessions() override { return nullptr; }
	virtual bool isHoliday(const char*, uint32_t, bool) override { return false; }
	virtual uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override { return uDate; }
	virtual uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool) override { return tDate; }

	virtual uint32_t getGlobalSize() override
	{
		++global_size_calls;
		return _global_size;
	}

	uint32_t contract_calls = 0;
	uint32_t global_size_calls = 0;

private:
	WTSContractInfo* _ct = nullptr;
	uint32_t _global_size = 8;
};

class R76ShmSink : public IParserSpi
{
public:
	explicit R76ShmSink(uint32_t globalSize = 8) { bd.setGlobalSize(globalSize); }

	virtual IBaseDataMgr* getBaseDataMgr() override
	{
		++bd_calls;
		return &bd;
	}

	virtual void handleSymbolList(const WTSArray*) override { ++symbol_lists; }
	virtual void handleQuote(WTSTickData*, uint32_t procFlag) override
	{
		++quotes;
		last_proc_flag = procFlag;
	}
	virtual void handleParserLog(WTSLogLevel, const char* message) override
	{
		logs.push_back(message != nullptr ? message : "");
	}
	virtual void handleEvent(WTSParserEvent, int32_t) override { ++events; }

	R76ShmBdMgr bd;
	uint32_t bd_calls = 0;
	uint32_t symbol_lists = 0;
	uint32_t quotes = 0;
	uint32_t events = 0;
	uint32_t last_proc_flag = 0;
	std::vector<std::string> logs;
};

/* ------------------------------------------------------------------ 夹具 */

class R76ParserShmTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		comm_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
		ct_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
		ASSERT_NE(nullptr, comm_);
		ASSERT_NE(nullptr, ct_);
		ct_->setCommInfo(comm_);
		ct_->setTotalIndex(3); // 必须 < getGlobalSize()
		sink_.bd.setContract(ct_);
	}

	void TearDown() override
	{
		parser_.release(); // 未 connect() -> _thrd_parser 为空，不会 join
		if (ct_ != nullptr)
		{
			ct_->release();
			ct_ = nullptr;
		}
		if (comm_ != nullptr)
		{
			comm_->release();
			comm_ = nullptr;
		}
	}

	WTSCommodityInfo* comm_ = nullptr;
	WTSContractInfo* ct_ = nullptr;
	ParserShm parser_;
	R76ShmSink sink_;
};

/* ------------------------------------------------------------------ 用例 */

/// init 可重复应用，且永远不建段/不起线程（ParserShm.cpp:76-83）
TEST_F(R76ParserShmTest, InitIsRepeatableAndNeverConnects)
{
	WTSVariant* full = WTSVariant::createObject();
	full->append("path", "/tmp/r76_shm_queue_a.dat");
	full->append("gpsize", (uint32_t)321);
	full->append("checkspan", (uint32_t)7);
	full->append("cpu", (uint32_t)1);

	WTSVariant* partial = WTSVariant::createObject();
	partial->append("path", "/tmp/r76_shm_queue_b.dat");

	EXPECT_TRUE(parser_.init(full));
	EXPECT_TRUE(parser_.init(partial)); // 第二次覆盖 path，仍只读配置
	full->release();
	partial->release();

	EXPECT_FALSE(parser_.isConnected());
	EXPECT_TRUE(sink_.logs.empty());
	EXPECT_EQ(0u, sink_.events);
}

/// registerSpi(NULL) 只清 _sink；下一次注册重新按「首次注册」处理，不再告警，
/// 但旧 _bd_mgr 仍被 subscribe 复用（ParserShm.cpp:315-329、284-298）
TEST_F(R76ParserShmTest, RegisterSpiNullKeepsOldBaseDataMgrAndSilencesNextReplacement)
{
	parser_.registerSpi(&sink_);
	ASSERT_EQ(1u, sink_.bd.global_size_calls);

	parser_.registerSpi(nullptr);
	ASSERT_TRUE(sink_.logs.empty());

	CodeSet afterNull;
	afterNull.insert("SHFE.ag2512");
	parser_.subscribe(afterNull);
	EXPECT_EQ(1u, sink_.bd.contract_calls); // 旧 bd_mgr 没被清空

	R76ShmSink second;
	second.bd.setContract(ct_);
	parser_.registerSpi(&second); // _sink 为空 -> bReplaced=false
	EXPECT_TRUE(second.logs.empty());
	EXPECT_EQ(1u, second.bd.global_size_calls);

	R76ShmSink third;
	third.bd.setContract(ct_);
	parser_.registerSpi(&third); // 真替换 -> 才告警
	ASSERT_EQ(1u, third.logs.size());
	EXPECT_NE(std::string::npos, third.logs[0].find("Listener is replaced"));
}

/// 注册时全局槽位为 0、订阅前才恢复：命中 286-287 的 resize 分支。
/// 这是本文件唯一新增行覆盖：既有用例在 registerSpi 时已 resize(8)，该分支从未执行。
TEST_F(R76ParserShmTest, SubscribeGrowsBitmapWhenBaseDataArrivesLate)
{
	ParserShm parser;
	R76ShmSink late(0); // getGlobalSize()==0 -> _subbed.resize(0)
	parser.registerSpi(&late);
	EXPECT_TRUE(late.logs.empty());

	late.bd.setGlobalSize(8); // 基础数据晚于 parser 注册就绪
	late.bd.setContract(ct_);

	CodeSet codes;
	codes.insert("SHFE.ag2512");
	parser.subscribe(codes); // _subbed.empty() -> resize(8)，再写 index 3
	EXPECT_EQ(1u, late.bd.contract_calls);

	parser.subscribe(codes); // 位图已非空，走另一个分支
	EXPECT_EQ(2u, late.bd.contract_calls);

	parser.unsubscribe(codes);
	EXPECT_EQ(3u, late.bd.contract_calls);
	EXPECT_EQ(0u, late.quotes);
	EXPECT_EQ(0u, late.events);
}

/// 空集合 / 未登记合约 / 重复代码：只查合约，不产生任何行情或事件
/// （ParserShm.cpp:284-298、301-313）
TEST_F(R76ParserShmTest, SubscribeAndUnsubscribeSkipUnknownCodesAndEmptySets)
{
	parser_.registerSpi(&sink_);

	CodeSet empty;
	parser_.subscribe(empty);
	parser_.unsubscribe(empty);
	EXPECT_EQ(0u, sink_.bd.contract_calls);

	CodeSet unknown;
	unknown.insert("DCE.a2509");
	parser_.subscribe(unknown);
	EXPECT_EQ(1u, sink_.bd.contract_calls);

	CodeSet known;
	known.insert("SHFE.ag2512");
	known.insert("SHFE.ag2512"); // set 去重，循环只走一次
	parser_.subscribe(known);
	EXPECT_EQ(2u, sink_.bd.contract_calls);
	parser_.unsubscribe(known);
	EXPECT_EQ(3u, sink_.bd.contract_calls);

	EXPECT_EQ(0u, sink_.quotes);
	EXPECT_EQ(0u, sink_.symbol_lists);
	EXPECT_EQ(0u, sink_.events);
	EXPECT_TRUE(sink_.logs.empty());
}

/// disconnect 只置 _stopped，不清合约表；之后的 subscribe 仍会查合约
/// （ParserShm.cpp:271-281 + 284-298，锁定「无 stopped 守卫」的现状）
TEST_F(R76ParserShmTest, DisconnectThenSubscribeStillResolvesCodes)
{
	parser_.registerSpi(&sink_);
	EXPECT_TRUE(parser_.disconnect());
	EXPECT_FALSE(parser_.isConnected());

	CodeSet codes;
	codes.insert("SHFE.ag2512");
	parser_.subscribe(codes);
	EXPECT_EQ(1u, sink_.bd.contract_calls);

	parser_.unsubscribe(codes);
	EXPECT_EQ(2u, sink_.bd.contract_calls);
	EXPECT_EQ(0u, sink_.events);
	EXPECT_TRUE(sink_.logs.empty());
}

/// 插件工厂 + 已注册 sink 的销毁：deleteParser 置空且二次调用安全
/// （ParserShm.cpp:43-59、72-74）
TEST_F(R76ParserShmTest, FactoryDeleteWithRegisteredSinkIsSafe)
{
	IParserApi* api = createParser();
	ASSERT_NE(nullptr, api);
	EXPECT_NE(nullptr, dynamic_cast<ParserShm*>(api));

	api->registerSpi(&sink_);
	CodeSet codes;
	codes.insert("SHFE.ag2512");
	api->subscribe(codes);
	EXPECT_EQ(1u, sink_.bd.contract_calls);

	deleteParser(api);
	EXPECT_EQ(nullptr, api);
	deleteParser(api); // 已为空，仍必须安全
	EXPECT_EQ(nullptr, api);
	EXPECT_EQ(0u, sink_.events);
}

/// IBaseDataMgr is a required sink dependency in the exercised parser setup.
TEST_F(R76ParserShmTest, DISABLED_NullBaseDataMgrContractPending)
{
	GTEST_SKIP() << "The supported ParserShm initialization contract requires a valid "
				 << "base data manager; no nullable dependency contract was found.";
}

} // namespace r76shm
