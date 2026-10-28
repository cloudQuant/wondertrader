// r162 / WtDataStorageAD 补测夹具（非用例 TU）
//
// 只提供最小宿主：IBaseDataMgr / IDataWriterSink / IDataReaderSink / IRdmDtReaderSink
// 与一个临时目录。全部走公开接口，不用访问权限宏，不镜像类布局。
// WtDataWriterAD 的 _async_task 在产品代码里没有初始化，writeTick 可能同步也可能排队，
// 因此夹具用「轮询 sink 计数」等待落库，不假设执行模型；writer 不 release（避免 join 未初始化线程）。
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IHotMgr.h"
#include "Includes/IDataReader.h"
#include "Includes/IDataWriter.h"
#include "Includes/IRdmDtReader.h"
#include "Includes/FasterDefs.h"
#include "WTSUtils/WTSCfgLoader.h"
#include "Share/StrUtil.hpp"
#include "Share/TimeUtils.hpp"
#include "Share/fmtlib.h"
#include "WtDataStorageAD/WtDataWriterAD.h"
#include "WtDataStorageAD/WtDataReaderAD.h"
#include "WtDataStorageAD/WtRdmDtReaderAD.h"

namespace r162ad {

using std::string;

class BaseDataMgr : public IBaseDataMgr
{
public:
	BaseDataMgr()
	{
		_sess = WTSSessionInfo::create("SSE_DAY", "r162 day", 0);
		_sess->addTradingSection(900, 1130);
		_sess->addTradingSection(1300, 1500);

		_comm = WTSCommodityInfo::create("m", "r162 mock", "SSE", "SSE_DAY", "CHN");
		_comm->setSessionInfo(_sess);
		_comm->setCategory(ContractCategory::CC_Stock);

		_ct = WTSContractInfo::create("600000", "600000", "SSE", "m");
		_ct->setCommInfo(_comm);
	}

	virtual ~BaseDataMgr()
	{
		_ct->release();
		_comm->release();
		_sess->release();
	}

	WTSContractInfo* contract() const { return _ct; }

	WTSCommodityInfo* getCommodity(const char*) override { return _comm; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return _comm; }
	WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
	{
		return (code != NULL && strcmp(code, "600000") == 0) ? _ct : NULL;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
	WTSSessionInfo* getSession(const char* = "") override { return _sess; }
	WTSSessionInfo* getSessionByCode(const char* = "", const char* = "") override { return _sess; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }

	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override
	{
		return uDate == 0 ? 20240110u : uDate;
	}

	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return (uint64_t)tDate * 1000000000ULL + (isStart ? 90000000ULL : 150000000ULL);
	}

private:
	WTSSessionInfo*		_sess = NULL;
	WTSCommodityInfo*	_comm = NULL;
	WTSContractInfo*	_ct = NULL;
};

class HotMgr : public IHotMgr
{
public:
	virtual ~HotMgr() {}

	const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isHot(const char*, const char*, uint32_t) override { return false; }
	bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isSecond(const char*, const char*, uint32_t) override { return false; }
	bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getRuleTag(const char*) override { return ""; }
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }
};

class WriterSink : public IDataWriterSink
{
public:
	explicit WriterSink(IBaseDataMgr* bd) : _bd(bd)
	{
		_comms.insert("SSE.m");
	}
	virtual ~WriterSink() {}

	IBaseDataMgr* getBDMgr() override { return _bd; }
	bool canSessionReceive(const char*) override { return true; }
	void broadcastTick(WTSTickData*) override { _ticks++; }
	void broadcastOrdQue(WTSOrdQueData*) override { _queues++; }
	void broadcastOrdDtl(WTSOrdDtlData*) override { _dtls++; }
	void broadcastTrans(WTSTransData*) override { _trans++; }
	CodeSet* getSessionComms(const char*) override { return &_comms; }
	uint32_t getTradingDate(const char*) override { return 20240110u; }

	void outputLog(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total++;
		_last = message == NULL ? "" : message;
	}

	uint32_t ticks() const { return _ticks; }
	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	const string& last_log() const { return _last; }

private:
	IBaseDataMgr*	_bd;
	CodeSet			_comms;
	uint32_t		_ticks = 0;
	uint32_t		_queues = 0;
	uint32_t		_dtls = 0;
	uint32_t		_trans = 0;
	uint32_t		_total = 0;
	string			_last;
	std::map<WTSLogLevel, uint32_t>	_logs;
};

class ReaderSink : public IDataReaderSink
{
public:
	ReaderSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~ReaderSink() {}

	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void on_bar(const char*, WTSKlinePeriod, WTSBarStruct*) override { _bars++; }
	void on_all_bar_updated(uint32_t) override { _updated++; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total++;
		_last = message == NULL ? "" : message;
	}
	uint32_t get_date() override { return 20240110u; }
	uint32_t get_min_time() override { return 1500u; }
	uint32_t get_secs() override { return 0u; }

	uint32_t bars() const { return _bars; }
	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	const string& last_log() const { return _last; }

private:
	IBaseDataMgr*	_bd;
	IHotMgr*		_hot;
	uint32_t		_bars = 0;
	uint32_t		_updated = 0;
	uint32_t		_total = 0;
	string			_last;
	std::map<WTSLogLevel, uint32_t>	_logs;
};

class RdmSink : public IRdmDtReaderSink
{
public:
	RdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~RdmSink() {}

	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total++;
		_last = message == NULL ? "" : message;
	}

	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	const string& last_log() const { return _last; }

private:
	IBaseDataMgr*	_bd;
	IHotMgr*		_hot;
	uint32_t		_total = 0;
	string			_last;
	std::map<WTSLogLevel, uint32_t>	_logs;
};

inline uint32_t bar_count(WTSKlineSlice* slice)
{
	uint32_t total = 0;
	if (slice == NULL)
		return 0;
	for (uint32_t i = 0; i < slice->get_block_counts(); i++)
		total += slice->get_block_size(i);
	return total;
}

inline uint32_t tick_count(WTSTickSlice* slice)
{
	uint32_t total = 0;
	if (slice == NULL)
		return 0;
	for (uint32_t i = 0; i < slice->get_block_counts(); i++)
		total += slice->get_block_size(i);
	return total;
}

class AdFixture : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		seq++;
		_dir = std::filesystem::temp_directory_path()
			/ fmtutil::format("r162_ad_{}_{}", (uint32_t)::getpid(), seq);
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
		ASSERT_FALSE(ec) << ec.message();

		_bd.reset(new BaseDataMgr());
		_wsink.reset(new WriterSink(_bd.get()));
		_rsink.reset(new ReaderSink(_bd.get(), &_hot));
		_qsink.reset(new RdmSink(_bd.get(), &_hot));
	}

	void TearDown() override
	{
		// writer 故意不 delete：_async_task 未初始化，若为真则 release 有丢唤醒窗口
		_wsink.reset();
		_rsink.reset();
		_qsink.reset();
		_cfgs.clear();
		_bd.reset();

		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	string base() const { return _dir.string() + "/"; }
	string path_of(const char* name) const { return base() + name; }

	string write_cfg(const char* name)
	{
		const string file = path_of(name);
		std::ofstream ofs(file.c_str(), std::ios::binary | std::ios::trunc);
		EXPECT_TRUE(ofs.is_open());
		ofs << "{\"path\":\"" << base() << "\",\"groupsize\":1000,"
			<< "\"disabletick\":false,\"disablemin1\":false,"
			<< "\"disablemin5\":false,\"disableday\":false}";
		ofs.close();
		_cfgs.push_back(file);
		return file;
	}

	WtDataWriterAD* make_writer()
	{
		const string file = write_cfg("writer.json");
		WTSVariant* cfg = WTSCfgLoader::load_from_file(file.c_str());
		EXPECT_TRUE(cfg != NULL) << file;
		if (cfg == NULL)
			return NULL;
		WtDataWriterAD* w = new WtDataWriterAD();
		EXPECT_TRUE(w->init(cfg, _wsink.get()));
		cfg->release();
		return w;
	}

	WtDataReaderAD* make_reader()
	{
		const string file = write_cfg("reader.json");
		WTSVariant* cfg = WTSCfgLoader::load_from_file(file.c_str());
		EXPECT_TRUE(cfg != NULL) << file;
		if (cfg == NULL)
			return NULL;
		WtDataReaderAD* r = new WtDataReaderAD();
		r->init(cfg, _rsink.get());
		cfg->release();
		return r;
	}

	WtRdmDtReaderAD* make_rdm_reader()
	{
		const string file = write_cfg("rdm.json");
		WTSVariant* cfg = WTSCfgLoader::load_from_file(file.c_str());
		EXPECT_TRUE(cfg != NULL) << file;
		if (cfg == NULL)
			return NULL;
		WtRdmDtReaderAD* r = new WtRdmDtReaderAD();
		r->init(cfg, _qsink.get());
		cfg->release();
		return r;
	}

	WTSTickData* make_tick(uint32_t tdate, uint32_t atime, double price, double volume)
	{
		WTSTickStruct ts;
		std::memset(&ts, 0, sizeof(ts));
		snprintf(ts.exchg, sizeof(ts.exchg), "%s", "SSE");
		snprintf(ts.code, sizeof(ts.code), "%s", "600000");
		ts.trading_date = tdate;
		ts.action_date = tdate;
		ts.action_time = atime;
		ts.price = price;
		ts.open = price;
		ts.high = price;
		ts.low = price;
		ts.settle_price = price;
		ts.total_volume = volume;
		ts.volume = 1.0;
		ts.total_turnover = price * volume;
		ts.turn_over = price;
		ts.open_interest = 1000.0 + volume;
		ts.pre_interest = 1000.0 + volume - 1.0;
		for (uint32_t i = 0; i < 10; i++)
		{
			ts.bid_prices[i] = price - 0.01 * (i + 1);
			ts.ask_prices[i] = price + 0.01 * (i + 1);
			ts.bid_qty[i] = 100.0 + i;
			ts.ask_qty[i] = 200.0 + i;
		}
		WTSTickData* ret = WTSTickData::create(ts);
		ret->setContractInfo(_bd->contract());
		return ret;
	}

	void push_tick(WtDataWriterAD* w, uint32_t tdate, uint32_t atime, double price, double volume)
	{
		ASSERT_TRUE(w != NULL);
		WTSTickData* d = make_tick(tdate, atime, price, volume);
		EXPECT_TRUE(w->writeTick(d, 0));
		d->release();
		_sent++;
		ASSERT_TRUE(wait_ticks(_sent)) << "writer did not broadcast tick " << _sent;
	}

	bool wait_ticks(uint32_t expected, uint32_t max_ms = 3000)
	{
		uint32_t waited = 0;
		while (waited < max_ms)
		{
			if (_wsink->ticks() >= expected)
				return true;
			usleep(5000);
			waited += 5;
		}
		return false;
	}

	std::filesystem::path	_dir;
	std::unique_ptr<BaseDataMgr>	_bd;
	std::unique_ptr<WriterSink>		_wsink;
	std::unique_ptr<ReaderSink>		_rsink;
	std::unique_ptr<RdmSink>		_qsink;
	HotMgr							_hot;
	std::vector<string>				_cfgs;
	uint32_t						_sent = 0;
};

} // namespace r162ad
