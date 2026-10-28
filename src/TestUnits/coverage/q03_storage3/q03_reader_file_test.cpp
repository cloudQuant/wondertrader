// q03 / WtDataReader：取数侧「文件形态」读取（cfg 绝对路径注入，零线程、零宏）
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - readTickSlice    :322  his/ticks/<exchg>/<endTDate>/<code>.dsb + rt/tickets 当日实时块
//                          四个版本位（RAW/CMP/RAW_V2/CMP_V2）读回、长度校验、尾部残缺、min(eIdx+1,count)
//  - readOrdQueSlice  :458 / readOrdDtlSlice :615 / readTransSlice :772  同型链路
//  - getRTTickBlock  :1920 / getRTOrdQueBlock :2024 / getRTOrdDtlBlock :1970 / getRTTransBlock :2078
//  - 多 his 目录（cfg.path 数组 + his_path）命中第二个目录
//
// 白盒手法：_rt_dir/_his_dir 由真实 json cfg 经公开 init() 注入（WtDataReader::init 不起线程），
// 宿主侧用 q03_storage3_fixture.h 的替身；没有任何访问权限宏。
#include "q03_storage3_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace q03 {

class Q03ReaderFileTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("rdf"));
		_root = _sandbox->root_abs();

		string cfgfile = _root + "reader.json";
		string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root);
		ASSERT_TRUE(write_bytes(cfgfile, json));

		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	static string his_path(const char* kind, uint32_t date, const char* code = "600000")
	{
		// reader 侧是绝对路径（cfg 注入），用例沙箱里等价地写相对路径的同一份文件
		return fmtutil::format("his/{}/{}/{}.dsb", kind, date, code);
	}

	void write_his(uint32_t date, uint16_t version, const string& payload, const char* kind = "ticks")
	{
		ASSERT_TRUE(write_bytes(his_path(kind, date), make_block(BT_HIS_Ticks, version, payload)));
	}

	void write_rt(const string& rel, const string& bytes)
	{
		ASSERT_TRUE(write_bytes(rel, bytes));
	}

	static vector<WTSTickStruct> flatten(WTSTickSlice* slice)
	{
		vector<WTSTickStruct> ret;
		if(slice == NULL)
			return ret;
		for(std::size_t i = 0; i < slice->get_block_counts(); i++)
		{
			WTSTickStruct* addr = slice->get_block_addr(i);
			uint32_t cnt = slice->get_block_size(i);
			if(addr == NULL)
				continue;
			for(uint32_t k = 0; k < cnt; k++)
				ret.push_back(addr[k]);
		}
		return ret;
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string		_root;
	BaseDataMgr	_bd;
	HotMgr		_hot;
	ReaderSink	_sink{ &_bd, &_hot };
	WtDataReader	_reader;
};

TEST_F(Q03ReaderFileTest, his_tick_raw_v2_roundtrip_and_count_window)
{
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(20240108, 20240108, 93000000, 1));
	ticks.push_back(make_tick(20240108, 20240108, 103000000, 2));
	ticks.push_back(make_tick(20240108, 20240108, 143000000, 3));
	write_his(20240108, BLOCK_VERSION_RAW_V2, pack_ticks(ticks));

	// etime 晚于全部 tick；count 给足 → 3 条全回
	WTSTickSlice* slice = _reader.readTickSlice("SSE.STK.600000", 100, 20240108150000000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), ticks.size());
	for(std::size_t i = 0; i < got.size(); i++)
		expect_tick_eq(got[i], ticks[i]);
	slice->release();

	// count==2 → min(eIdx+1, count) 收敛，取最后 2 条
	WTSTickSlice* few = _reader.readTickSlice("SSE.STK.600000", 2, 20240108150000000ull);
	ASSERT_TRUE(few != NULL);
	vector<WTSTickStruct> got2 = flatten(few);
	ASSERT_EQ(got2.size(), (std::size_t)2);
	expect_tick_eq(got2[0], ticks[1]);
	expect_tick_eq(got2[1], ticks[2]);
	few->release();
}

TEST_F(Q03ReaderFileTest, his_tick_version_matrix)
{
	// CMP_V2：新结构体 + 压缩
	vector<WTSTickStruct> a;
	for(uint32_t i = 0; i < 12; i++)
		a.push_back(make_tick(20240108, 20240108, 93000000 + i * 1000, i + 1));
	write_his(20240108, BLOCK_VERSION_CMP_V2, pack_ticks(a));
	WTSTickSlice* s1 = _reader.readTickSlice("SSE.STK.600000", 50, 20240108150000000ull);
	ASSERT_TRUE(s1 != NULL);
	vector<WTSTickStruct> g1 = flatten(s1);
	ASSERT_EQ(g1.size(), a.size());
	for(std::size_t i = 0; i < g1.size(); i++)
		expect_tick_eq(g1[i], a[i]);
	s1->release();

	// RAW：老结构体未压缩 → 升版
	vector<WTSTickStructOld> b;
	b.push_back(make_old_tick(20240109, 20240109, 93100000, 1));
	b.push_back(make_old_tick(20240109, 20240109, 140000000, 2));
	write_his(20240109, BLOCK_VERSION_RAW, pack_old_ticks(b));
	WTSTickSlice* s2 = _reader.readTickSlice("SSE.STK.600000", 50, 20240109150000000ull);
	ASSERT_TRUE(s2 != NULL);
	vector<WTSTickStruct> g2 = flatten(s2);
	ASSERT_EQ(g2.size(), b.size());
	for(std::size_t i = 0; i < g2.size(); i++)
		expect_tick_upgraded(g2[i], b[i]);
	s2->release();

	// CMP：老结构体 + 压缩 → 解压再升版
	vector<WTSTickStructOld> c;
	c.push_back(make_old_tick(20240111, 20240111, 93200000, 1));
	write_his(20240111, BLOCK_VERSION_CMP, pack_old_ticks(c));
	WTSTickSlice* s3 = _reader.readTickSlice("SSE.STK.600000", 50, 20240111150000000ull);
	ASSERT_TRUE(s3 != NULL);
	vector<WTSTickStruct> g3 = flatten(s3);
	ASSERT_EQ(g3.size(), c.size());
	expect_tick_upgraded(g3[0], c[0]);
	s3->release();
}

TEST_F(Q03ReaderFileTest, his_tick_size_check_and_tail_fragment)
{
	// 6 字节 < sizeof(HisTickBlock)(12) → :418 Sizechecking → NULL
	write_his(20240108, BLOCK_VERSION_RAW_V2, string(6, 'X'));
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600000", 10, 20240108150000000ull) == NULL);
	EXPECT_GE(_sink.count_of(LL_ERROR), 1u);

	// 覆盖成「2 条完整 + 20 字节残缺」
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(20240109, 20240109, 93000000, 1));
	ticks.push_back(make_tick(20240109, 20240109, 143000000, 2));
	string payload = pack_ticks(ticks);
	payload.append(20, 'K');
	// 同一个 key 已缓存过（残缺文件写在 20240108，这里换交易日重新建库）
	write_his(20240109, BLOCK_VERSION_RAW_V2, payload);
	WTSTickSlice* slice = _reader.readTickSlice("SSE.STK.600000", 10, 20240109150000000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	ASSERT_EQ(got.size(), (std::size_t)2);
	expect_tick_eq(got[0], ticks[0]);
	expect_tick_eq(got[1], ticks[1]);
	slice->release();
}

TEST_F(Q03ReaderFileTest, today_branch_reads_rt_tick_block)
{
	// curTDate==20240110（替身固定），endTDate==curTDate → isToday → rt/ticks/SSE/600000.dmb
	vector<WTSTickStruct> rt;
	rt.push_back(make_tick(20240110, 20240110, 93000000, 1));
	rt.push_back(make_tick(20240110, 20240110, 103000000, 2));
	rt.push_back(make_tick(20240110, 20240110, 143000000, 3));
	write_rt("rt/ticks/SSE/600000.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240110, (uint32_t)rt.size(), 8, rt));

	WTSTickSlice* slice = _reader.readTickSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSTickStruct> got = flatten(slice);
	// min(eIdx+1, count)=2，取末尾 2 条
	ASSERT_EQ(got.size(), (std::size_t)2);
	expect_tick_eq(got[0], rt[1]);
	expect_tick_eq(got[1], rt[2]);
	slice->release();

	// 当日但 rt 文件不存在 → getRTTickBlock 返回 NULL → :367 return NULL
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600999", 2, 20240110150000000ull) == NULL);
}

TEST_F(Q03ReaderFileTest, order_queue_detail_and_transaction_files)
{
	vector<WTSOrdQueStruct> que;
	que.push_back(make_ordque(20240108, 20240108, 93000000, 1));
	que.push_back(make_ordque(20240108, 20240108, 143000000, 2));
	string f_que = "his/queue/SSE/20240108/600000.dsb";
	ASSERT_TRUE(write_bytes(f_que, make_block(BT_HIS_OrdQueue, BLOCK_VERSION_RAW_V2, pack_items(que))));

	WTSOrdQueSlice* sq = _reader.readOrdQueSlice("SSE.STK.600000", 10, 20240108150000000ull);
	ASSERT_TRUE(sq != NULL);
	ASSERT_EQ(sq->size(), 2u);
	for(uint32_t i = 0; i < sq->size(); i++)
	{
		const WTSOrdQueStruct* it = sq->at((int32_t)i);
		ASSERT_TRUE(it != NULL);
		EXPECT_EQ(it->action_date, que[i].action_date);
		EXPECT_EQ(it->action_time, que[i].action_time);
		EXPECT_DOUBLE_EQ(it->price, que[i].price);
		EXPECT_EQ(it->order_items, que[i].order_items);
		EXPECT_EQ(it->qsize, que[i].qsize);
		EXPECT_STREQ(it->code, que[i].code);
		for(uint32_t k = 0; k < 50; k++)
			EXPECT_EQ(it->volumes[k], que[i].volumes[k]);
	}
	sq->release();

	vector<WTSOrdDtlStruct> dtl;
	dtl.push_back(make_orddtl(20240108, 20240108, 93000000, 3));
	string f_dtl = "his/orders/SSE/20240108/600000.dsb";
	ASSERT_TRUE(write_bytes(f_dtl, make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtl))));
	WTSOrdDtlSlice* sd = _reader.readOrdDtlSlice("SSE.STK.600000", 10, 20240108150000000ull);
	ASSERT_TRUE(sd != NULL);
	ASSERT_EQ(sd->size(), 1u);
	{
		const WTSOrdDtlStruct* it = sd->at(0);
		ASSERT_TRUE(it != NULL);
		EXPECT_EQ(it->index, dtl[0].index);
		EXPECT_DOUBLE_EQ(it->price, dtl[0].price);
		EXPECT_EQ(it->volume, dtl[0].volume);
		EXPECT_EQ(it->orderid, dtl[0].orderid);
	}
	sd->release();

	vector<WTSTransStruct> trs;
	trs.push_back(make_trans(20240108, 20240108, 93000000, 4));
	trs.push_back(make_trans(20240108, 20240108, 143000000, 5));
	string f_trs = "his/trans/SSE/20240108/600000.dsb";
	ASSERT_TRUE(write_bytes(f_trs, make_block(BT_HIS_Trnsctn, BLOCK_VERSION_RAW_V2, pack_items(trs))));
	WTSTransSlice* st = _reader.readTransSlice("SSE.STK.600000", 10, 20240108150000000ull);
	ASSERT_TRUE(st != NULL);
	ASSERT_EQ(st->size(), 2u);
	for(uint32_t i = 0; i < st->size(); i++)
	{
		const WTSTransStruct* it = st->at((int32_t)i);
		ASSERT_TRUE(it != NULL);
		EXPECT_EQ(it->index, trs[i].index);
		EXPECT_DOUBLE_EQ(it->price, trs[i].price);
		EXPECT_EQ(it->volume, trs[i].volume);
		EXPECT_EQ(it->askorder, trs[i].askorder);
		EXPECT_EQ(it->bidorder, trs[i].bidorder);
	}
	st->release();
}

TEST_F(Q03ReaderFileTest, missing_his_file_returns_null_without_log)
{
	// 非当日、文件缺失 → :412 return NULL（不打错误日志，静默）
	uint32_t err_before = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600111", 5, 20240105150000000ull) == NULL);
	EXPECT_EQ(_sink.count_of(LL_ERROR), err_before);
}

} // namespace q03
