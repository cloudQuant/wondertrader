/*
 * L05 - WtDtHelper：Tick / 委托明细 / 委托队列 / 逐笔成交 的存储与读取
 *
 * 覆盖 src/WtDtHelper/WtDtHelper.cpp 中 store_ticks / store_order_details /
 * store_order_queues / store_transactions / read_dsb_ticks / read_dsb_order_details /
 * read_dsb_order_queues / read_dsb_transactions / read_dmb_ticks / dump_ticks。
 *
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/l05_dt_servo_porter.md。
 */
#include <gtest/gtest.h>

#include "WtDtHelper/WtDtHelper.h"
#include "Includes/WTSDataDef.hpp"
#include "WtDataStorage/DataDefine.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

USING_NS_WTP;

namespace {

	std::vector<std::string>& t_log_sink()
	{
		static std::vector<std::string> s_logs;
		return s_logs;
	}

	std::vector<WTSTickStruct>& tick_sink()
	{
		static std::vector<WTSTickStruct> s_ticks;
		return s_ticks;
	}

	std::vector<char>& raw_sink()
	{
		static std::vector<char> s_raw;
		return s_raw;
	}

	uint32_t& t_count_sink()
	{
		static uint32_t s_cnt = 0;
		return s_cnt;
	}

	void t_log_cb(WtString message)
	{
		t_log_sink().assign(1, message == NULL ? std::string("<null>") : std::string(message));
	}

	void ticks_cb(WTSTickStruct* tick, WtUInt32 count, bool isLast)
	{
		if (tick != NULL && count > 0)
			tick_sink().assign(tick, tick + count);
		(void)isLast;
	}

	void orddtl_cb(WTSOrdDtlStruct* item, WtUInt32 count, bool isLast)
	{
		if (item != NULL && count > 0)
			raw_sink().assign((char*)item, (char*)item + count * sizeof(WTSOrdDtlStruct));
		(void)isLast;
	}

	void ordque_cb(WTSOrdQueStruct* item, WtUInt32 count, bool isLast)
	{
		if (item != NULL && count > 0)
			raw_sink().assign((char*)item, (char*)item + count * sizeof(WTSOrdQueStruct));
		(void)isLast;
	}

	void trans_cb(WTSTransStruct* item, WtUInt32 count, bool isLast)
	{
		if (item != NULL && count > 0)
			raw_sink().assign((char*)item, (char*)item + count * sizeof(WTSTransStruct));
		(void)isLast;
	}

	void t_count_cb(WtUInt32 dataCnt)
	{
		t_count_sink() = dataCnt;
	}

	bool t_any_log_has(const char* key)
	{
		for (const auto& s : t_log_sink())
		{
			if (s.find(key) != std::string::npos)
				return true;
		}
		return false;
	}

	WTSTickStruct make_tick(uint32_t actTime, double price)
	{
		WTSTickStruct tick;
		memset(&tick, 0, sizeof(WTSTickStruct));
		strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
		strncpy(tick.code, "rb2010", sizeof(tick.code) - 1);
		tick.trading_date = 20200116;
		tick.action_date = 20200116;
		tick.action_time = actTime;
		tick.price = price;
		tick.open = price - 1;
		tick.high = price + 2;
		tick.low = price - 2;
		tick.settle_price = price;
		tick.pre_close = price - 1;
		tick.pre_settle = price - 1;
		tick.pre_interest = 1000;
		tick.total_volume = 5000;
		tick.total_turnover = 870000000;
		tick.open_interest = 12000;
		tick.volume = 100;
		tick.turn_over = 17400000;
		tick.diff_interest = 10;
		for (int i = 0; i < 10; i++)
		{
			tick.bid_prices[i] = price - 0.5 * (i + 1);
			tick.bid_qty[i] = 10 + i;
			tick.ask_prices[i] = price + 0.5 * (i + 1);
			tick.ask_qty[i] = 20 + i;
		}
		return tick;
	}

	// 用可辨识的字节序列填充未知结构体，避免依赖未确认的字段名
	template <typename ITEM>
	std::vector<ITEM> make_pattern_items(size_t count)
	{
		std::vector<ITEM> items(count);
		for (size_t i = 0; i < count; i++)
		{
			unsigned char* bytes = (unsigned char*)&items[i];
			for (size_t b = 0; b < sizeof(ITEM); b++)
				bytes[b] = (unsigned char)((i * 31 + b * 7) % 251);
		}
		return items;
	}

} // namespace

class L05HelperTickTest : public ::testing::Test
{
protected:
	std::filesystem::path _root;

	void SetUp() override
	{
		static int s_seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("l05_helper_tick_" + std::to_string(::getpid()) + "_" + std::to_string(++s_seq));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root, ec);
		t_log_sink().clear();
		tick_sink().clear();
		raw_sink().clear();
		t_count_sink() = 0;
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	std::string file(const std::string& name) const
	{
		return (_root / name).string();
	}
};

TEST_F(L05HelperTickTest, StoreTicksRejectsZeroCount)
{
	EXPECT_FALSE(store_ticks(file("none.dst").c_str(), NULL, 0, t_log_cb));
	EXPECT_TRUE(t_any_log_has("0"));
	EXPECT_FALSE(store_ticks(file("none.dst").c_str(), NULL, 0, NULL));
	EXPECT_FALSE(std::filesystem::exists(_root / "none.dst"));
}

TEST_F(L05HelperTickTest, StoreAndReadTicksRoundtrip)
{
	std::vector<WTSTickStruct> ticks;
	for (uint32_t i = 0; i < 4; i++)
		ticks.push_back(make_tick(93000 + i, 3000 + i));

	ASSERT_TRUE(store_ticks(file("SHFE.rb2010.dsb").c_str(), ticks.data(), (int)ticks.size(), t_log_cb));
	ASSERT_TRUE(std::filesystem::exists(_root / "SHFE.rb2010.dsb"));
	EXPECT_TRUE(t_any_log_has("Tick"));

	tick_sink().clear();
	t_count_sink() = 0;
	WtUInt32 reaCnt = read_dsb_ticks(file("SHFE.rb2010.dsb").c_str(), ticks_cb, t_count_cb, t_log_cb);
	EXPECT_EQ(reaCnt, (WtUInt32)ticks.size());
	EXPECT_EQ(t_count_sink(), (uint32_t)ticks.size());
	ASSERT_EQ(tick_sink().size(), ticks.size());
	EXPECT_STREQ(tick_sink()[0].exchg, "SHFE");
	EXPECT_STREQ(tick_sink()[2].code, "rb2010");
	EXPECT_DOUBLE_EQ(tick_sink()[3].price, 3003);
	EXPECT_DOUBLE_EQ(tick_sink()[1].bid_prices[0], 3001.5);
	EXPECT_EQ(tick_sink()[0].action_time, 93000u);
	EXPECT_TRUE(t_any_log_has("读取完成"));
}

TEST_F(L05HelperTickTest, ReadTicksHeaderValidation)
{
	{
		std::ofstream ofs(file("tiny.dsb"), std::ios::binary | std::ios::trunc);
		ofs << "abc";
	}
	t_count_sink() = 99;
	tick_sink().clear();
	EXPECT_EQ(read_dsb_ticks(file("tiny.dsb").c_str(), ticks_cb, t_count_cb, t_log_cb), (WtUInt32)0);
	EXPECT_TRUE(t_any_log_has("头部校验失败"));
	EXPECT_EQ(t_count_sink(), 99u);
	EXPECT_TRUE(tick_sink().empty());

	EXPECT_EQ(read_dsb_ticks(file("missing.dsb").c_str(), ticks_cb, t_count_cb, NULL), (WtUInt32)0);
}

TEST_F(L05HelperTickTest, StoreAndReadOrdDetailRoundtrip)
{
	auto items = make_pattern_items<WTSOrdDtlStruct>(3);
	ASSERT_TRUE(store_order_details(file("orddtl.dsb").c_str(), items.data(), (int)items.size(), t_log_cb));
	EXPECT_TRUE(t_any_log_has("order detail"));

	raw_sink().clear();
	t_count_sink() = 0;
	WtUInt32 reaCnt = read_dsb_order_details(file("orddtl.dsb").c_str(),
		orddtl_cb, t_count_cb, t_log_cb);
	EXPECT_EQ(reaCnt, (WtUInt32)items.size());
	EXPECT_EQ(t_count_sink(), (uint32_t)items.size());
	ASSERT_EQ(raw_sink().size(), items.size() * sizeof(WTSOrdDtlStruct));
	EXPECT_EQ(memcmp(raw_sink().data(), items.data(), raw_sink().size()), 0);

	EXPECT_FALSE(store_order_details(file("orddtl.dsb").c_str(), items.data(), 0, t_log_cb));
	EXPECT_FALSE(store_order_details(file("orddtl.dsb").c_str(), items.data(), 0, NULL));
}

TEST_F(L05HelperTickTest, StoreAndReadOrdQueueRoundtrip)
{
	auto items = make_pattern_items<WTSOrdQueStruct>(5);
	ASSERT_TRUE(store_order_queues(file("ordque.dsb").c_str(), items.data(), (int)items.size(), NULL));

	raw_sink().clear();
	t_count_sink() = 0;
	WtUInt32 reaCnt = read_dsb_order_queues(file("ordque.dsb").c_str(),
		ordque_cb, t_count_cb, t_log_cb);
	EXPECT_EQ(reaCnt, (WtUInt32)items.size());
	ASSERT_EQ(raw_sink().size(), items.size() * sizeof(WTSOrdQueStruct));
	EXPECT_EQ(memcmp(raw_sink().data(), items.data(), raw_sink().size()), 0);
	EXPECT_TRUE(t_any_log_has("order queue"));

	EXPECT_FALSE(store_order_queues(file("ordque.dsb").c_str(), items.data(), 0, t_log_cb));
	// 头部校验失败分支
	t_count_sink() = 8;
	EXPECT_EQ(read_dsb_order_queues(file("tiny.dsb").c_str(), (FuncGetOrdQueCallback)&ordque_cb, t_count_cb, NULL), (WtUInt32)0);
	EXPECT_EQ(t_count_sink(), 8u);
}

TEST_F(L05HelperTickTest, StoreAndReadTransactionRoundtrip)
{
	auto items = make_pattern_items<WTSTransStruct>(2);
	ASSERT_TRUE(store_transactions(file("trans.dsb").c_str(), items.data(), (int)items.size(), t_log_cb));
	EXPECT_TRUE(t_any_log_has("transactions"));

	raw_sink().clear();
	WtUInt32 reaCnt = read_dsb_transactions(file("trans.dsb").c_str(),
		(FuncGetTransCallback)&trans_cb, t_count_cb, t_log_cb);
	EXPECT_EQ(reaCnt, (WtUInt32)items.size());
	ASSERT_EQ(raw_sink().size(), items.size() * sizeof(WTSTransStruct));
	EXPECT_EQ(memcmp(raw_sink().data(), items.data(), raw_sink().size()), 0);

	EXPECT_FALSE(store_transactions(file("trans.dsb").c_str(), items.data(), 0, t_log_cb));
	EXPECT_FALSE(store_transactions(file("trans.dsb").c_str(), items.data(), 0, NULL));
	EXPECT_EQ(read_dsb_transactions(file("missing.dsb").c_str(), trans_cb, t_count_cb, NULL), (WtUInt32)0);
}

TEST_F(L05HelperTickTest, ReadDmbTicksPaths)
{
	{
		std::ofstream ofs(file("tiny.dmb"), std::ios::binary | std::ios::trunc);
		ofs << "12";
	}
	t_count_sink() = 5;
	EXPECT_EQ(read_dmb_ticks(file("tiny.dmb").c_str(), ticks_cb, t_count_cb, t_log_cb), (WtUInt32)0);
	EXPECT_TRUE(t_any_log_has("头部校验失败"));
	EXPECT_EQ(t_count_sink(), 5u);

	std::string blob(sizeof(RTTickBlock), '\0');
	((RTTickBlock*)&blob[0])->_size = 0;
	{
		std::ofstream ofs(file("zero.dmb"), std::ios::binary | std::ios::trunc);
		ofs.write(blob.data(), (std::streamsize)blob.size());
	}
	t_count_sink() = 0;
	EXPECT_EQ(read_dmb_ticks(file("zero.dmb").c_str(), ticks_cb, t_count_cb, NULL), (WtUInt32)0);
	EXPECT_EQ(t_count_sink(), 0u);

	blob.resize(sizeof(RTTickBlock) + 2 * sizeof(WTSTickStruct));
	RTTickBlock* block = (RTTickBlock*)&blob[0];
	block->_size = 2;
	block->_date = 20200116;
	block->_ticks[0] = make_tick(93000, 3001);
	block->_ticks[1] = make_tick(93001, 3002);
	{
		std::ofstream ofs(file("two.dmb"), std::ios::binary | std::ios::trunc);
		ofs.write(blob.data(), (std::streamsize)blob.size());
	}
	tick_sink().clear();
	t_count_sink() = 0;
	EXPECT_EQ(read_dmb_ticks(file("two.dmb").c_str(), ticks_cb, t_count_cb, t_log_cb), (WtUInt32)2);
	EXPECT_EQ(t_count_sink(), 2u);
	ASSERT_EQ(tick_sink().size(), (size_t)2);
	EXPECT_DOUBLE_EQ(tick_sink()[1].price, 3002);
	EXPECT_TRUE(t_any_log_has("读取完成"));
}

TEST_F(L05HelperTickTest, DumpTicksWritesCsvAndSkipsInvalid)
{
	std::vector<WTSTickStruct> ticks;
	for (uint32_t i = 0; i < 3; i++)
		ticks.push_back(make_tick(93000 + i, 3000 + i));
	ASSERT_TRUE(store_ticks((_root / "SHFE.rb2010.dsb").string().c_str(), ticks.data(), (int)ticks.size(), NULL));

	{
		std::ofstream ofs((_root / "broken.dsb").string(), std::ios::binary | std::ios::trunc);
		ofs << "xy";
	}
	std::ofstream ignore((_root / "ignore.txt").string());
	ignore << "ignored";
	std::filesystem::create_directories(_root / "sub_dir");

	const std::string csvDir = (_root / "tick_csv").string();
	dump_ticks(_root.string().c_str(), csvDir.c_str(), "", t_log_cb);
	EXPECT_TRUE(t_any_log_has("头部校验失败"));
	EXPECT_TRUE(t_any_log_has("导出完成"));

	const std::string csv = (_root / "tick_csv" / "SHFE.rb2010.csv").string();
	ASSERT_TRUE(std::filesystem::exists(csv));
	std::ifstream ifs(csv);
	std::string line;
	ASSERT_TRUE(std::getline(ifs, line));
	EXPECT_NE(line.find("bidprice10"), std::string::npos);
	EXPECT_NE(line.find("askqty10"), std::string::npos);
	int dataLines = 0;
	while (std::getline(ifs, line))
	{
		if (!line.empty())
		{
			if (dataLines == 0)
			{
				EXPECT_NE(line.find("SHFE"), std::string::npos);
				EXPECT_NE(line.find("rb2010"), std::string::npos);
			}
			dataLines++;
		}
	}
	EXPECT_EQ(dataLines, 3);

	// 源目录不存在
	t_log_sink().clear();
	dump_ticks((_root / "nowhere").string().c_str(), csvDir.c_str(), "", t_log_cb);
	EXPECT_TRUE(t_any_log_has("不存在"));
}
