// r94 / WtDataReader：扩展加载器的分钟周期名、RT K 线扩容重映射与分钟闭合缺块
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - cacheFinalBarsFromLoader 的 pname 分支 KP_Minute1/KP_Minute5 :943-946
//  - getRTKilneBlock 的 `_last_cap != _capacity` 重映射分支       :2191-2205
//  - onMinuteEnd 中 getRTKilneBlock==NULL 的 continue             :2224-2226
//
// 加载器用 r52::Loader（内存 final bars），reader 用真实 init(cfg, sink, loader)；
// 临时目录由 q03::Sandbox 管理，无墙钟/后台线程。
#include "r52_storage_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace r94 {

using namespace q03;
using std::string;
using std::vector;

class R94ReaderLoaderRtTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r94ldr"));
		_root = _sandbox->root_abs();

		vector<WTSBarStruct> bars;
		bars.push_back(make_bar(20240109, r52::min_bar_time(20240109, 930), 1));
		bars.push_back(make_bar(20240109, r52::min_bar_time(20240109, 931), 2));
		_loader.add_final("SSE.STK.600020", bars);

		const string cfgfile = _root + "reader.json";
		ASSERT_TRUE(write_bytes(cfgfile, fmtutil::format("{{\"path\":\"{}\"}}", _root)));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, &_loader);
		cfg->release();
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string				_root;
	r52::BDMgr			_bd;
	HotMgr				_hot;
	ReaderSink			_sink{ &_bd, &_hot };
	r52::Loader			_loader;
	WtDataReader			_reader;
};

// WtDataReader.cpp: L943-946
TEST_F(R94ReaderLoaderRtTest, R94ReaderLoaderServesMin1AndMin5FinalBars)
{
	WTSKlineSlice* m1 = _reader.readKlineSlice("SSE.STK.600020", KP_Minute1, 2,
		202401101500ull);
	ASSERT_TRUE(m1 != NULL);
	EXPECT_EQ(r52::flatten_bars(m1).size(), (std::size_t)2);
	m1->release();

	WTSKlineSlice* m5 = _reader.readKlineSlice("SSE.STK.600020", KP_Minute5, 2,
		202401101500ull);
	ASSERT_TRUE(m5 != NULL);
	EXPECT_EQ(r52::flatten_bars(m5).size(), (std::size_t)2);
	m5->release();
}

// WtDataReader.cpp: L2191-2205 / L2224-2226
TEST_F(R94ReaderLoaderRtTest, R94ReaderRtKlineRemapThenMinuteEndSkipsMissingBlock)
{
	vector<WTSBarStruct> rt;
	rt.push_back(make_bar(20240110, r52::min_bar_time(20240110, 930), 11));
	rt.push_back(make_bar(20240110, r52::min_bar_time(20240110, 931), 12));
	ASSERT_TRUE(write_bytes("rt/min1/SSE/600022.dmb",
		make_rt_block<WTSBarStruct>(BT_RT_Minute1, 20240110, 2, 8, rt)));

	WTSKlineSlice* first = _reader.readKlineSlice("SSE.STK.600022", KP_Minute1, 2,
		202401101500ull);
	ASSERT_TRUE(first != NULL);
	ASSERT_EQ(r52::flatten_bars(first).size(), (std::size_t)2);
	first->release();

	// 写侧扩容后读侧必须重新映射才能看到新的 capacity
	ASSERT_TRUE(r52::patch_rt_capacity("rt/min1/SSE/600022.dmb", 12));

	WTSKlineSlice* second = _reader.readKlineSlice("SSE.STK.600022", KP_Minute1, 2,
		202401101500ull);
	ASSERT_TRUE(second != NULL);
	ASSERT_EQ(r52::flatten_bars(second).size(), (std::size_t)2);
	second->release();

	// 移除 RT 文件后分钟闭合：缓存中的周期块找不到 RT -> continue，仅触发汇总回调
	std::error_code ec;
	std::filesystem::remove("rt/min1/SSE/600022.dmb", ec);
	_reader.onMinuteEnd(20240110, 1500);
}

} // namespace r94
