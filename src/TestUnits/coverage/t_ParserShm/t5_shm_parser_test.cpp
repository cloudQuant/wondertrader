/*
 * T5 lane — src/ParserShm/ParserShm.cpp 覆盖率用例（收发族 / 共享内存接收）
 *
 * 特别说明（按 lane 要求）：ParserShm 的接收状态机整段写在 connect() 起的线程里
 * （ParserShm.cpp:92-269），且必须先 BoostMappingFile::map() 出一个真实映射文件才能拿到
 * _queue。提交 c3525f22 已经把「环境相关」的 shm 用例摘掉过一次，所以本轮：
 *   - 不做 shm_open / boost::interprocess，不建真实共享内存段，不起线程；
 *   - 只覆盖纯逻辑可完成的面：配置解析返回值、连接状态判定、registerSpi 的
 *     「监听者被替换」分支、subscribe/unsubscribe 的标准码拆分→(原始代码,交易所)换算、
 *     以及厂商共用的队列头/条目布局（ParserShm.h 的 DataItem / _DataQueue<N>，
 *     它们是 public 嵌套类型，可以在普通堆内存上直接构造并断言布局）；
 *   - 必须真段才能进的面（_readable/_writable 追赶循环、NODATA_FLAG 复位、
 *     lastIdx%capacity 取模落位、四类 _type 分发、_cpu 绑核、pid 变化重置）
 *     整条留给主控裁定，见 docs/迭代计划/迭代01/bugs/T5_yd_xtp_ctp_shm.md。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ParserShm/ParserShm.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"

USING_NS_WTP;

extern "C"
{
	IParserApi* createParser();
	void deleteParser(IParserApi*& parser);
}

namespace {

class ShmFakeBdMgr : public IBaseDataMgr
{
public:
	virtual ~ShmFakeBdMgr() {}

	void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
	{
		ct->setCommInfo(comm);
		_comms.push_back(comm);
		_cts.push_back(ct);
		_cts2.push_back(ct);
		_ct_key[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
	}

	virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		for (auto* c : _comms)
			if (strcmp(c->getFullPid(), exchgpid) == 0)
				return c;
		return NULL;
	}
	virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{ (void)exchg; (void)pid; return NULL; }

	virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
	{
		(void)uDate;
		++n_get_contract_;
		calls_.push_back(std::string(code ? code : "") + "@" + (exchg ? exchg : ""));
		auto it = _ct_key.find(std::string(code ? code : "") + "|" + (exchg ? exchg : ""));
		return it == _ct_key.end() ? NULL : it->second;
	}

	virtual WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override
	{ (void)exchg; (void)uDate; return NULL; }
	virtual WTSSessionInfo* getSession(const char* sid) override { (void)sid; return NULL; }
	virtual WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
	{ (void)code; (void)exchg; return NULL; }
	virtual WTSArray* getAllSessions() override { return NULL; }
	virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override
	{ (void)pid; (void)uDate; (void)isTpl; return false; }
	virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
	{ (void)stdPID; (void)uTime; (void)isSession; return uDate; }
	virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
	{ (void)stdPID; (void)isSession; (void)isStart; return tDate; }
	/// 全局槽位数量：决定 _subbed 的尺寸，必须 > 最大 totalIndex，否则下标越界
	virtual uint32_t getGlobalSize() override { ++n_global_size_; return 8; }

	uint32_t n_get_contract_ = 0;
	uint32_t n_global_size_ = 0;
	std::vector<std::string> calls_;

private:
	std::vector<WTSCommodityInfo*> _comms;
	std::vector<WTSContractInfo*> _cts;
	std::vector<WTSContractInfo*> _cts2;
	wt_hashmap<std::string, WTSContractInfo*> _ct_key;
};

class ShmSink : public IParserSpi
{
public:
	virtual ~ShmSink() {}

	virtual IBaseDataMgr* getBaseDataMgr() override { return &bd_; }
	virtual void handleSymbolList(const WTSArray* aySymbols) override
	{ ++n_symbol_list_; last_symbol_size_ = aySymbols ? aySymbols->size() : 0; }
	virtual void handleQuote(WTSTickData* quote, uint32_t procFlag) override { (void)quote; ++n_quote_; last_proc_flag_ = procFlag; }
	virtual void handleParserLog(WTSLogLevel ll, const char* message) override
	{ logs_.push_back({ (int)ll, message ? message : "" }); }
	virtual void handleEvent(WTSParserEvent e, int32_t ec) override { events_.push_back({ (int)e, ec }); }

	struct LogRec { int ll; std::string msg; };
	struct EventRec { int e; int32_t ec; };

	uint32_t n_quote_ = 0;
	uint32_t n_symbol_list_ = 0;
	uint32_t n_login_events_ = 0;
	uint32_t last_symbol_size_ = 0;
	uint32_t last_proc_flag_ = 0;
	std::vector<LogRec> logs_;
	std::vector<EventRec> events_;

	ShmFakeBdMgr bd_;
};

WTSVariant* makeShmCfg(bool full)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", "/tmp/t5_shm_queue.dat");
	if (full)
	{
		cfg->append("gpsize", (uint32_t)500);
		cfg->append("checkspan", (uint32_t)200);
		cfg->append("cpu", (uint32_t)2);
	}
	return cfg;
}

class ParserShmTest : public ::testing::Test
{
protected:
	ParserShm parser_;
	ShmSink sink_;
	WTSCommodityInfo* comm_ = NULL;
	WTSContractInfo* ct_ = NULL;
	WTSCommodityInfo* option_comm_ = NULL;
	WTSContractInfo* option_ct_ = NULL;

	void SetUp() override
	{
		ct_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
		ct_->setTotalIndex(3);                       // 必须 < getGlobalSize()=8
		comm_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
		comm_->setCategory(CC_Future);
		comm_->setVolScale(5);
		sink_.bd_.regis(comm_, ct_);
		// 仓内合法的多段期权标准码：CFFEX.IO2013.C.4000；原始码为 IO2013.C.4000。
		option_ct_ = WTSContractInfo::create("IO2013.C.4000", "IO2013 C 4000", "CFFEX", "IO");
		option_ct_->setTotalIndex(4);
		option_comm_ = WTSCommodityInfo::create("IO", "IO", "CFFEX", "TRADING", "CHINA");
		option_comm_->setCategory(CC_SpotOption);
		option_comm_->setVolScale(1);
		sink_.bd_.regis(option_comm_, option_ct_);
		parser_.registerSpi(&sink_);
	}

	void TearDown() override
	{
		parser_.disconnect();
		if (option_ct_) option_ct_->release();
		if (option_comm_) option_comm_->release();
		if (ct_) ct_->release();
		if (comm_) comm_->release();
	}
};

/* ---------------------------------------------------------------- 用例 */

/// init 正/负配置：只读参数并返回 true，不建段、不起线程
TEST_F(ParserShmTest, init_accepts_full_and_partial_config)
{
	WTSVariant* full = makeShmCfg(true);
	WTSVariant* partial = makeShmCfg(false);
	EXPECT_TRUE(parser_.init(full));
	EXPECT_TRUE(parser_.init(partial));
	full->release();
	partial->release();

	// init 不碰共享内存：状态仍是未连接，且没有任何事件/日志外发
	EXPECT_FALSE(parser_.isConnected());
	EXPECT_TRUE(sink_.events_.empty());
	EXPECT_TRUE(sink_.logs_.empty());
}

/// isConnected/disconnect 的纯状态判定（_queue 恒 NULL，因为不 connect）
TEST_F(ParserShmTest, connection_state_and_disconnect_idempotent)
{
	EXPECT_FALSE(parser_.isConnected());
	EXPECT_TRUE(parser_.disconnect());
	EXPECT_TRUE(parser_.disconnect());
	EXPECT_FALSE(parser_.isConnected());
	// release() = disconnect() + join 线程；_thrd_parser 为空 → 不解引用
	EXPECT_NO_THROW(parser_.release());
}

/// registerSpi：首次注册不告警，第二次注册才写「Listener is replaced」
TEST_F(ParserShmTest, register_spi_warns_only_on_replacement)
{
	// SetUp 已经注册过一次 → 没有 replaced 告警
	EXPECT_TRUE(sink_.logs_.empty());
	// 且把 IBaseDataMgr 拉了过来并按 getGlobalSize 开了订阅位图
	EXPECT_EQ(1u, sink_.bd_.n_global_size_);

	ShmSink second;
	second.bd_.regis(comm_, ct_);
	parser_.registerSpi(&second);
	ASSERT_EQ(1u, second.logs_.size());
	EXPECT_NE(std::string::npos, second.logs_[0].msg.find("Listener is replaced"));
	EXPECT_EQ(1u, second.bd_.n_global_size_);     // 替换后仍会重新取一次全局尺寸
	parser_.registerSpi(&sink_);
}

/// subscribe：标准码 "交易所.原始代码" 拆成 (原始代码, 交易所) 去查合约
TEST_F(ParserShmTest, subscribe_splits_std_code_into_raw_code_and_exchange)
{
	CodeSet codes;
	codes.insert("SHFE.ag2512");
	parser_.subscribe(codes);

	ASSERT_EQ(1u, sink_.bd_.calls_.size());
	EXPECT_EQ("ag2512@SHFE", sink_.bd_.calls_[0]);     // ay[1]@ay[0]
	EXPECT_EQ(0u, sink_.n_quote_);                     // 订阅本身不产生行情

	// 未登记的合约 → getContract NULL → continue，不写订阅位
	CodeSet unknown;
	unknown.insert("DCE.a2509");
	parser_.subscribe(unknown);
	ASSERT_EQ(2u, sink_.bd_.calls_.size());
	EXPECT_EQ("a2509@DCE", sink_.bd_.calls_[1]);
}

/// 多段标准期权码必须保留完整原始代码；查询路径不得丢弃后续段
TEST_F(ParserShmTest, subscribe_and_unsubscribe_preserve_multisegment_standard_code)
{
	CodeSet codes;
	codes.insert("CFFEX.IO2013.C.4000");
	parser_.subscribe(codes);
	ASSERT_EQ(1u, sink_.bd_.calls_.size());
	EXPECT_EQ("IO2013.C.4000@CFFEX", sink_.bd_.calls_[0]);
	EXPECT_EQ(0u, sink_.n_quote_);

	parser_.unsubscribe(codes);
	ASSERT_EQ(2u, sink_.bd_.calls_.size());
	EXPECT_EQ("IO2013.C.4000@CFFEX", sink_.bd_.calls_[1]);
}

/// unsubscribe 与 subscribe 走同一套拆分；不依赖 _subbed 尺寸（已 registerSpi）
TEST_F(ParserShmTest, unsubscribe_splits_the_same_way)
{
	CodeSet codes;
	codes.insert("SHFE.ag2512");
	parser_.unsubscribe(codes);
	ASSERT_EQ(1u, sink_.bd_.calls_.size());
	EXPECT_EQ("ag2512@SHFE", sink_.bd_.calls_[0]);

	CodeSet unknown;
	unknown.insert("CZCE.SF509");
	parser_.unsubscribe(unknown);
	ASSERT_EQ(2u, sink_.bd_.calls_.size());
	EXPECT_EQ("SF509@CZCE", sink_.bd_.calls_[1]);
	EXPECT_EQ(0u, sink_.n_quote_);
}

/// 空集合订阅：不查任何合约、不产生回报
TEST_F(ParserShmTest, subscribe_empty_set_touches_nothing)
{
	CodeSet empty;
	parser_.subscribe(empty);
	parser_.unsubscribe(empty);
	EXPECT_EQ(0u, sink_.bd_.n_get_contract_);
	EXPECT_TRUE(sink_.events_.empty());
}

/* ------------------------------------------- 队列布局（纯内存，不建段） */

/// DataItem 默认构造把整条记录清零（接收侧靠这个语义判断空槽）
TEST(shm_queue_layout, data_item_default_ctor_zeroes_whole_record)
{
	ParserShm::DataItem* item = new ParserShm::DataItem();
	EXPECT_EQ(0u, item->_type);
	EXPECT_EQ(0.0, item->_tick.price);
	EXPECT_EQ(0, item->_tick.exchg[0]);
	EXPECT_EQ(0, item->_queue.code[0]);
	EXPECT_EQ(0, item->_order.exchg[0]);
	memset(&item->_tick, 0xAB, sizeof(item->_tick));
	EXPECT_EQ(0xAB, (uint8_t)item->_queue.code[0]);      // union 成员共享同一块内存
	delete item;
}

/// 四个 union 成员必须同址（收发两端的线格式约定）
TEST(shm_queue_layout, union_members_alias_at_same_address)
{
	ParserShm::DataItem* item = new ParserShm::DataItem();
	void* a = (void*)&item->_tick;
	void* b = (void*)&item->_queue;
	void* c = (void*)&item->_order;
	void* d = (void*)&item->_trans;
	EXPECT_EQ(a, b);
	EXPECT_EQ(a, c);
	EXPECT_EQ(a, d);
	EXPECT_GE(sizeof(ParserShm::DataItem), sizeof(WTSTickStruct) + sizeof(uint32_t));
	delete item;
}

/// 队列头字段顺序与尺寸：接收循环用 _readable%capacity 定位，靠的就是这个布局
TEST(shm_queue_layout, queue_header_fields_and_item_offset)
{
	ParserShm::CastQueue* q = new ParserShm::CastQueue();
	EXPECT_EQ(8u * 1024u, (uint32_t)q->_capacity);       // 默认模板参数 N=8*1024
	EXPECT_EQ(UINT64_MAX, q->_readable);                // 「还没有数据」的哨兵
	EXPECT_EQ(0u, q->_writable);
	EXPECT_EQ(0u, q->_pid);

	// pack(8)：三个 uint64 + 一个 uint32 之后，8 字节对齐的 items 落在偏移 32
	const uintptr_t base = (uintptr_t)&q->_capacity;
	const uintptr_t items = (uintptr_t)&q->_items[0];
	EXPECT_EQ((uintptr_t)32, items - base);
	// 取模落位一定在数组内（接收循环 lastIdx % _capacity）
	EXPECT_LT(&q->_items[q->_capacity - 1], &q->_items[0] + q->_capacity);
	EXPECT_GE((uintptr_t)&q->_items[q->_capacity - 1], items);
	delete q;
}

/// 自定义容量模板：同一份代码可以给队列换尺寸，capacity 与数组长度必须一致
TEST(shm_queue_layout, template_queue_honours_custom_capacity)
{
	const uint32_t N = 16;
	ParserShm::_DataQueue<16>* q = new ParserShm::_DataQueue<16>();
	EXPECT_EQ((uint64_t)N, q->_capacity);
	// 环形回绕：idx=17 落在槽位 1
	const uint64_t idx = 17;
	EXPECT_EQ((uint64_t)1, idx % q->_capacity);
	// 槽位地址在数组范围内
	EXPECT_LT((uintptr_t)&q->_items[idx % q->_capacity] - (uintptr_t)&q->_items[0],
		(uintptr_t)(sizeof(ParserShm::DataItem) * N));
	delete q;
}

/* ------------------------------------------------------- 不能进的路径 */

/// 未 registerSpi 时 _bd_mgr 是构造未初始化的野指针（见 bugs），subscribe 直接解引用
TEST(ParserShmNoSpi, DISABLED_subscribe_without_register_spi_uses_uninit_bd_mgr)
{
	ParserShm p;
	WTSVariant* cfg = makeShmCfg(false);
	ASSERT_TRUE(p.init(cfg));
	cfg->release();
	CodeSet codes;
	codes.insert("SHFE.ag2512");
	p.subscribe(codes);
	p.disconnect();
}

/// 零容量基础数据管理器没有已登记合约；查找返回空后不应访问订阅位图。
class ZeroSizeShmSink : public ShmSink
{
public:
	virtual IBaseDataMgr* getBaseDataMgr() override { return &_mgr; }

	class Mgr : public ShmFakeBdMgr
	{
	public:
		virtual uint32_t getGlobalSize() override
		{
			++n_global_size_;
			return 0;
		}
	} _mgr;
};

TEST(ParserShmZeroGlobalSize, no_contract_lookup_skips_subscription_index)
{
	ParserShm p;
	ZeroSizeShmSink sink;
	p.registerSpi(&sink);
	CodeSet codes;
	codes.insert("SHFE.ag2512");
	p.subscribe(codes);

	// 零个槽位意味着没有可查到的合约，subscribe 应跳过索引写入。
	ASSERT_EQ(1u, sink._mgr.n_get_contract_);
	EXPECT_EQ("ag2512@SHFE", sink._mgr.calls_[0]);
	EXPECT_EQ(0u, sink.n_quote_);
	EXPECT_TRUE(sink.events_.empty());
	p.disconnect();
}

/// 接收状态机（_readable 追赶 / pid 复位 / _type 四类分发 / _cpu 绑核）
/// 必须先 map 出一个真实的 BoostMappingFile 才能进入 → 本轮按 lane 约定不建段，留给主控裁定。
TEST(ParserShmReceiveLoop, DISABLED_receive_path_needs_a_real_mapped_file)
{
	ParserShm p;
	EXPECT_TRUE(p.connect());        // 会起线程 + 等文件存在（sleep 2s 循环）→ 不可在门禁内运行
}

/* ---------------------------------------------------------------- 工厂 */

TEST(shm_plugin_factory, create_and_delete_roundtrip_null_safe)
{
	IParserApi* p = createParser();
	ASSERT_NE(nullptr, p);
	EXPECT_FALSE(p->isConnected());
	EXPECT_TRUE(p->disconnect());
	p->release();
	deleteParser(p);
	EXPECT_EQ(nullptr, p);

	IParserApi* nil = NULL;
	deleteParser(nil);
	EXPECT_EQ(nullptr, nil);
}

} // namespace
