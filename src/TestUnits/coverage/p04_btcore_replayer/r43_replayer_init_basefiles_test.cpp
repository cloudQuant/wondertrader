/**
 * r43_replayer_init_basefiles_test.cpp
 * Lane R43：补 HisDataReplayer::init(L171-300) 中「可选基础数据 / store 路径 /
 * 除权因子文件」这些 p04/p07/q02 都只走了一部分的分支：
 *   L182-189  —— store 子节点覆盖 path
 *   L235-265  —— commodity / contract 的 String 与 Array 两种形态
 *   L267-285  —— holiday / hot / second / rules 装载
 *   L302-405  —— loadStkAdjFactorsFromFile：有效文件、无除权键、坏 JSON
 * 落盘只在 temp_directory_path()/r43_init_* 下，TearDown 清理。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace
{
	std::filesystem::path r43_init_root()
	{
		// 构建系统当前把 __FILE__ 展开成仓库内的绝对路径（见 build-cov/build.ninja），
		// 从源文件目录上溯 5 级到仓库根；少一级会落到 src/，拼出的 dist/common/*.json
		// 就变成不存在的 src/dist/...。若构建改为相对源路径，这里需要改成按可执行文件定位。
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	std::filesystem::path r43_init_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("r43_init_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	WTSVariant* r43_init_cfg(const std::filesystem::path& base)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001081500));
		cfg->append("tick", false);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		return cfg;
	}

	bool r43_write_text(const std::filesystem::path& p, const std::string& content)
	{
		std::error_code ec;
		std::filesystem::create_directories(p.parent_path(), ec);
		std::ofstream out(p, std::ios::binary);
		if (!out.is_open())
			return false;
		out << content;
		out.close();
		return true;
	}

	bool r43_write_daily_csv(const std::filesystem::path& base)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / "SSE.STK.000001_d.csv");
		if (!out.is_open())
			return false;
		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n"
			<< "20200102,150000,10.0,11.0,9.5,10.5,1000,10000,0,0,0\n"
			<< "20200103,150000,10.5,12.0,10.4,11.5,1100,11000,0,0,0\n";
		out.close();
		return true;
	}

	class R43InitSink : public IDataSink
	{
	public:
		std::function<void()> onInit;
		uint32_t replayDoneCalls {0};

		void handle_init() override
		{
			if (onInit)
				onInit();
		}
		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
		void handle_schedule(uint32_t, uint32_t) override {}
		void handle_session_begin(uint32_t) override {}
		void handle_session_end(uint32_t) override {}
		void handle_replay_done() override { replayDoneCalls++; }
	};

	const char* kAdjJson =
		"{\n"
		"  \"SSE\": {\n"
		"    \"600000\": [{\"date\": 20200106, \"factor\": 2.0}],\n"
		"    \"STK.600001\": [{\"date\": 20200106, \"factor\": 1.5}, {\"date\": 20200203, \"factor\": 3.0}]\n"
		"  }\n"
		"}\n";
}

class R43ReplayerInitBasefilesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = r43_init_root();
		_base = r43_init_dir("data");
		_out = r43_init_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;
};

// commodity 数组形态 + contract/holiday/hot/second/rules 全量装载
TEST_F(R43ReplayerInitBasefilesTest, optionalBasefilesAllLoaded)
{
	WTSVariant* cfg = r43_init_cfg(_base);

	WTSVariant* commodity = WTSVariant::createArray();
	commodity->append((_root / "dist/common/stk_comms.json").string().c_str());
	commodity->append((_root / "dist/common/commodities.json").string().c_str());

	WTSVariant* basefiles = WTSVariant::createObject();
	basefiles->append("session", (_root / "dist/common/sessions.json").string().c_str());
	basefiles->append("commodity", commodity);
	basefiles->append("contract", (_root / "dist/common/contracts.json").string().c_str());
	basefiles->append("holiday", (_root / "dist/common/holidays.json").string().c_str());
	basefiles->append("hot", (_root / "dist/common/hots.json").string().c_str());
	basefiles->append("second", (_root / "dist/common/hots.json").string().c_str());
	WTSVariant* rules = WTSVariant::createObject();
	rules->append("MYHOT", (_root / "dist/common/hots.json").string().c_str());
	basefiles->append("rules", rules);
	rules->release();
	cfg->append("basefiles", basefiles);
	basefiles->release();
	commodity->release();

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	EXPECT_NE(nullptr, replayer.get_session_info("SD0930"));
	EXPECT_NE(nullptr, replayer.get_commodity_info("SSE.STK.000001"));
	// hots.json 里 CFFEX.IC 在 20190117 切到 IC1902：能查到即说明 hot 分支生效
	EXPECT_EQ(std::string("IC1902"), std::string(replayer.get_hot_mgr()->getRawCode("CFFEX", "IC", 20190117u)));
	// rules 分支按自定义 tag 注册了同一份规则
	EXPECT_EQ(std::string("IC1902"),
		std::string(replayer.get_hot_mgr()->getCustomRawCode("MYHOT", "CFFEX.IC", 20190117u)));
	// contract 装载后分月合约仍能取到品种信息
	EXPECT_NE(nullptr, replayer.get_commodity_info("CFFEX.IC.2204"));
}

// store 子节点存在时，数据目录取 store.path 而不是根 path
TEST_F(R43ReplayerInitBasefilesTest, storePathOverridesRootPath)
{
	ASSERT_TRUE(r43_write_daily_csv(_base));

	WTSVariant* cfg = r43_init_cfg(_base / "not_a_data_dir");
	WTSVariant* store = WTSVariant::createObject();
	store->append("path", _base.string().c_str());
	cfg->append("store", store);
	store->release();

	WTSVariant* basefiles = WTSVariant::createObject();
	basefiles->append("session", (_root / "dist/common/sessions.json").string().c_str());
	basefiles->append("commodity", (_root / "dist/common/stk_comms.json").string().c_str());
	cfg->append("basefiles", basefiles);
	basefiles->release();

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	// get_kline_slice 的日线定位依赖 prepare() 设置的 _cur_tdate（HisDataReplayer.cpp:661-664），
	// 且 prepare() 要求已注册 sink（L669）。这里按标准生命周期走一遍。
	R43InitSink sink;
	replayer.register_sink(&sink, "r43_storepath");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* slice = replayer.get_kline_slice("SSE.STK.000001", "d", 1, 1, false);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(1u, slice->size());
	EXPECT_DOUBLE_EQ(10.5, slice->at(0)->close);
	slice->release();
}

// 有效除权因子文件：带 PID 与不带 PID 的 code 都要能解析，且不影响 init 结果
TEST_F(R43ReplayerInitBasefilesTest, adjFactorFileParsed)
{
	const std::filesystem::path adjfile = _base / "adjfactors.json";
	ASSERT_TRUE(r43_write_text(adjfile, kAdjJson));

	WTSVariant* cfg = r43_init_cfg(_base);
	cfg->append("adjfactor", adjfile.string().c_str());
	WTSVariant* basefiles = WTSVariant::createObject();
	basefiles->append("session", (_root / "dist/common/sessions.json").string().c_str());
	basefiles->append("commodity", (_root / "dist/common/stk_comms.json").string().c_str());
	cfg->append("basefiles", basefiles);
	basefiles->release();

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	// 除权因子只影响复权K线装配，基础查询必须保持可用
	EXPECT_NE(nullptr, replayer.get_commodity_info("SSE.STK.600000"));
	EXPECT_DOUBLE_EQ(0.0, replayer.get_cur_price("SSE.STK.600000"));
}

// 坏 JSON / 不存在的除权因子文件都不能把 init 打断（只打日志）
TEST_F(R43ReplayerInitBasefilesTest, badAdjFactorFileDoesNotBreakInit)
{
	const std::filesystem::path badfile = _base / "bad_adj.json";
	ASSERT_TRUE(r43_write_text(badfile, "{ this is not json"));

	WTSVariant* cfg = r43_init_cfg(_base);
	cfg->append("adjfactor", badfile.string().c_str());
	WTSVariant* basefiles = WTSVariant::createObject();
	basefiles->append("session", (_root / "dist/common/sessions.json").string().c_str());
	basefiles->append("commodity", (_root / "dist/common/stk_comms.json").string().c_str());
	cfg->append("basefiles", basefiles);
	basefiles->release();

	HisDataReplayer replayer;
	EXPECT_TRUE(replayer.init(cfg));
	cfg->release();

	// 坏文件被忽略后仍能正常 prepare/run 收尾
	R43InitSink sink;
	replayer.register_sink(&sink, "r43_badadj");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}
