// r112 / WtDataReader：股票复权 min5 分支与尺寸/续接守卫
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - cacheAdjustedStkBars 的 KP_Minute5 周期名 :1300
//  - 复权文件过短 -> break 后回落原始文件 :1346-1350
//  - 原始文件缺失 continue :1425；原始文件过短 return false :1429-1433
//  - 原始文件解出空 buffer break :1439-1440
//  - 因子倒序回放里 pBar->date < bar.date 的 continue :1497-1498
//
// init(cfg) 的 adjfactor 分支由 WtDataReader 自己加载（不起线程）。
#include "r52_storage_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace r112a {

using namespace q03;
using std::string;
using std::vector;

class R112ReaderAdjustedMin5Test : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r112adj"));
		_root = _sandbox->root_abs();
		_bd.set_cur_tdate(20240110);
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	void init_reader(const char* extra)
	{
		const string cfgfile = _root + "reader.json";
		const string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0{}}}", _root, extra);
		ASSERT_TRUE(write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}

	void seed_adjusted(const char* code, uint32_t d1, uint32_t d2)
	{
		vector<WTSBarStruct> adj;
		adj.push_back(make_bar(d1, TimeUtils::timeToMinBar(d1, 1455), 1));
		adj.push_back(make_bar(d2, TimeUtils::timeToMinBar(d2, 1455), 2));
		ASSERT_TRUE(write_bytes(string("his/min5/SSE/") + code + "-.dsb",
			make_block(BT_HIS_Minute5, BLOCK_VERSION_RAW_V2, pack_bars(adj))));
	}

	void seed_raw(const char* code, const vector<WTSBarStruct>& bars)
	{
		ASSERT_TRUE(write_bytes(string("his/min5/SSE/") + code + ".dsb",
			make_block(BT_HIS_Minute5, BLOCK_VERSION_RAW_V2, pack_bars(bars))));
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string				_root;
	r52::BDMgr			_bd;
	r52::HotMgr			_hot;
	r52::ReaderSink			_sink{ &_bd, &_hot };
	WtDataReader			_reader;
};

// WtDataReader.cpp: L1300 / L1346-1350 / L1425 / L1429-1433 / L1439-1440 / L1497-1498
TEST_F(R112ReaderAdjustedMin5Test, R112ReaderAdjustedMin5ContinuationAndGuards)
{
	const string adjfile = _root + "adj.json";
	ASSERT_TRUE(write_bytes(adjfile,
		"{\"SSE\":{\"600900\":[{\"date\":20240103,\"factor\":1.1},"
		"{\"date\":20240104,\"factor\":1.2},{\"date\":20240201,\"factor\":1.3}]}}"));

	// 1) 复权文件 + 原始文件续接：原始段从 lastQTime 之后开始
	seed_adjusted("600900", 20240102, 20240103);
	vector<WTSBarStruct> raw;
	raw.push_back(make_bar(20240103, TimeUtils::timeToMinBar(20240103, 1455), 11));
	raw.push_back(make_bar(20240104, TimeUtils::timeToMinBar(20240104, 935), 12));
	seed_raw("600900", raw);

	// 2) 复权文件 8 字节（< HisKlineBlock）→ break 后回落原始文件
	ASSERT_TRUE(write_bytes("his/min5/SSE/600901-.dsb", string(8, 'X')));
	vector<WTSBarStruct> raw2;
	raw2.push_back(make_bar(20240103, TimeUtils::timeToMinBar(20240103, 1455), 21));
	raw2.push_back(make_bar(20240104, TimeUtils::timeToMinBar(20240104, 935), 22));
	seed_raw("600901", raw2);

	// 3) 复权文件正常、原始文件缺失 → continue
	seed_adjusted("600902", 20240102, 20240103);

	// 4) 复权文件正常、原始文件 8 字节 → return false
	seed_adjusted("600903", 20240102, 20240103);
	ASSERT_TRUE(write_bytes("his/min5/SSE/600903.dsb", string(8, 'Y')));

	// 5) 复权文件正常、原始文件 CMP 空载荷 → 解出空 buffer 后 break
	seed_adjusted("600904", 20240102, 20240103);
	seed_raw("600904", vector<WTSBarStruct>());

	const string extra = fmtutil::format(",\"adjfactor\":\"{}\"", adjfile);
	init_reader(extra.c_str());

	// 1) 2 条复权 + 1 条原始续接
	WTSKlineSlice* s1 = _reader.readKlineSlice("SSE.STK.600900-", KP_Minute5, 100, 202401051500ull);
	ASSERT_TRUE(s1 != NULL);
	vector<WTSBarStruct> got1 = r52::flatten_bars(s1);
	ASSERT_EQ(got1.size(), (std::size_t)3);
	EXPECT_EQ(got1[0].date, 20240102u);
	EXPECT_EQ(got1[1].date, 20240103u);
	EXPECT_EQ(got1[2].date, 20240104u);
	s1->release();

	// 2) 复权文件过短 -> 回落原始文件 2 条
	WTSKlineSlice* s2 = _reader.readKlineSlice("SSE.STK.600901-", KP_Minute5, 100, 202401051500ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(r52::flatten_bars(s2).size(), (std::size_t)2);
	s2->release();

	// 3) 原始文件缺失 -> 仅复权段 2 条
	WTSKlineSlice* s3 = _reader.readKlineSlice("SSE.STK.600902-", KP_Minute5, 100, 202401051500ull);
	ASSERT_TRUE(s3 != NULL);
	EXPECT_EQ(r52::flatten_bars(s3).size(), (std::size_t)2);
	s3->release();

	// 4) 原始文件过短 -> cacheAdjustedStkBars 返回 false，缓存为空
	WTSKlineSlice* s4 = _reader.readKlineSlice("SSE.STK.600903-", KP_Minute5, 100, 202401051500ull);
	ASSERT_TRUE(s4 != NULL);
	EXPECT_EQ(s4->size(), 0u);
	s4->release();

	// 5) 原始文件空载荷 -> break，保留复权段 2 条
	WTSKlineSlice* s5 = _reader.readKlineSlice("SSE.STK.600904-", KP_Minute5, 100, 202401051500ull);
	ASSERT_TRUE(s5 != NULL);
	EXPECT_EQ(r52::flatten_bars(s5).size(), (std::size_t)2);
	s5->release();
}

} // namespace r112a
