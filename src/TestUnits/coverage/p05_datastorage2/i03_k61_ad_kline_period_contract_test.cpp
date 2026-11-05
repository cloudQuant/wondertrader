// 迭代03 K61（BUG-I02-NEW-060，用户裁定 2026-11-06）：K 线读端只支持 m1/m5/day，AD 读端同一合同。
//
// WtDataReaderAD::readKlineSlice、WtRdmDtReaderAD::readKlineSliceByRange/ByCount 收到其他周期时
// 返回 NULL，并打一条 ERROR“Unsupported kline period”；基础周期不受影响。
#include "r4_ad_fixture.h"

#include "WtDataStorageAD/WtDataReaderAD.h"
#include "WtDataStorageAD/WtRdmDtReaderAD.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace i03k61ad {

using namespace r4fx;

namespace {

class LogSink
{
public:
	void add(WTSLogLevel ll, const char* message)
	{
		if (ll == LL_ERROR)
			_errors++;
		_last = message == NULL ? "" : message;
	}
	uint32_t _errors = 0;
	std::string _last;
};

class ReaderSink : public IDataReaderSink
{
public:
	explicit ReaderSink(IBaseDataMgr* bd) : _bd(bd) {}
	virtual ~ReaderSink() {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return NULL; }
	void on_bar(const char*, WTSKlinePeriod, WTSBarStruct*) override {}
	void on_all_bar_updated(uint32_t) override {}
	void reader_log(WTSLogLevel ll, const char* message) override { _log.add(ll, message); }
	uint32_t get_date() override { return 20230103u; }
	uint32_t get_min_time() override { return 1500u; }
	uint32_t get_secs() override { return 0u; }
	IBaseDataMgr* _bd;
	LogSink _log;
};

class RdmSink : public IRdmDtReaderSink
{
public:
	explicit RdmSink(IBaseDataMgr* bd) : _bd(bd) {}
	virtual ~RdmSink() {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return NULL; }
	void reader_log(WTSLogLevel ll, const char* message) override { _log.add(ll, message); }
	IBaseDataMgr* _bd;
	LogSink _log;
};

} // namespace

class I03K61AdKlinePeriodTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = std::filesystem::temp_directory_path()
			/ ("i03_k61_ad_" + std::to_string((uint32_t)::getpid()) + "_" +
				::testing::UnitTest::GetInstance()->current_test_info()->name());
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root, ec);
		ASSERT_FALSE(ec) << ec.message();
		_bd.reset(new R4BaseDataMgr());
		WTSCommodityInfo* comm = _bd->add_commodity("EXQ", "rb2301", ContractCategory::CC_Stock);
		_bd->add_contract("EXQ", "rb2301", comm);
		_sink.reset(new ReaderSink(_bd.get()));
		_rsink.reset(new RdmSink(_bd.get()));

		_reader.reset(new WtDataReaderAD());
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _root.string().c_str());
		_reader->init(cfg, _sink.get(), NULL);
		_rdm.reset(new WtRdmDtReaderAD());
		_rdm->init(cfg, _rsink.get());
		cfg->release();
	}

	void TearDown() override
	{
		_rdm.reset();
		_reader.reset();
		_rsink.reset();
		_sink.reset();
		_bd.reset();
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	std::filesystem::path			_root;
	std::unique_ptr<R4BaseDataMgr>	_bd;
	std::unique_ptr<ReaderSink>		_sink;
	std::unique_ptr<RdmSink>		_rsink;
	std::unique_ptr<WtDataReaderAD>	_reader;
	std::unique_ptr<WtRdmDtReaderAD>	_rdm;
};

TEST_F(I03K61AdKlinePeriodTest, NonBasePeriodsAreRejectedWithError)
{
	const WTSKlinePeriod bad[] = { KP_Tick, KP_Hour, KP_Half, (WTSKlinePeriod)99 };
	for (WTSKlinePeriod period : bad)
	{
		uint32_t before = _sink->_log._errors;
		WTSKlineSlice* slice = _reader->readKlineSlice("EXQ.rb2301", period, 3, 202301031500ull);
		EXPECT_EQ(nullptr, slice) << "reader period " << (uint32_t)period;
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(before + 1, _sink->_log._errors) << "reader period " << (uint32_t)period;
		EXPECT_NE(std::string::npos, _sink->_log._last.find("Unsupported kline period")) << _sink->_log._last;

		before = _rsink->_log._errors;
		slice = _rdm->readKlineSliceByRange("EXQ.rb2301", period, 202301030900ull, 202301031500ull);
		EXPECT_EQ(nullptr, slice) << "range period " << (uint32_t)period;
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(before + 1, _rsink->_log._errors) << "range period " << (uint32_t)period;
		EXPECT_NE(std::string::npos, _rsink->_log._last.find("Unsupported kline period")) << _rsink->_log._last;

		before = _rsink->_log._errors;
		slice = _rdm->readKlineSliceByCount("EXQ.rb2301", period, 3, 202301031500ull);
		EXPECT_EQ(nullptr, slice) << "count period " << (uint32_t)period;
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(before + 1, _rsink->_log._errors) << "count period " << (uint32_t)period;
		EXPECT_NE(std::string::npos, _rsink->_log._last.find("Unsupported kline period")) << _rsink->_log._last;
	}
}

TEST_F(I03K61AdKlinePeriodTest, BasePeriodsAreNotRejected)
{
	const WTSKlinePeriod good[] = { KP_Minute1, KP_Minute5, KP_DAY };
	for (WTSKlinePeriod period : good)
	{
		WTSKlineSlice* slice = _reader->readKlineSlice("EXQ.rb2301", period, 3, 202301031500ull);
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(std::string::npos, _sink->_log._last.find("Unsupported kline period")) << (uint32_t)period;
		slice = _rdm->readKlineSliceByRange("EXQ.rb2301", period, 202301030900ull, 202301031500ull);
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(std::string::npos, _rsink->_log._last.find("Unsupported kline period")) << (uint32_t)period;
		slice = _rdm->readKlineSliceByCount("EXQ.rb2301", period, 3, 202301031500ull);
		if (slice != NULL)
			slice->release();
		EXPECT_EQ(std::string::npos, _rsink->_log._last.find("Unsupported kline period")) << (uint32_t)period;
	}
}

} // namespace i03k61ad
