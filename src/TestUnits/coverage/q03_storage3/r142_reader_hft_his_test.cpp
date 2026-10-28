// r142 / WtDataReader：历史逐笔计数读取的右界回退、历史 tick 空块第二次访问
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - readOrdQueSlice 历史段右界回退 :601-606
//  - readTransSlice  历史段右界回退 :915-920
//  - readTickSlice   历史块为空时第二次访问 :427-429
//
// 逐笔历史文件是 CMP_V2，但载荷为不可压缩样本（r142_fixture.h），
// 保证 resize+append 不重分配，避开 BUG-R132-001 同型的悬垂写。
// 路径由公开 init(cfg) 注入，全部位于 q03::Sandbox 临时目录，零线程。
#include "r142_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace r142 {

using namespace q03;
using std::string;
using std::vector;

class R142ReaderHftHisTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r142rd"));
		_root = _sandbox->root_abs();

		string cfgfile = "reader.json";
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

	std::unique_ptr<Sandbox>	_sandbox;
	string				_root;
	BaseDataMgr			_bd;
	HotMgr				_hot;
	ReaderSink			_sink{ &_bd, &_hot };
	WtDataReader			_reader;
};

// WtDataReader.cpp: readOrdQueSlice L594-611（L603-606）/ readTransSlice L908-925（L917-920）
TEST_F(R142ReaderHftHisTest, R142ReaderHisHftCountBacksOffPastLastItem)
{
	const uint32_t kT = 20240109;
	const uint32_t kPrev = 20240108;

	// 两条记录都比 15:00 早：lower_bound 的搜索区间是 [begin, begin+(tcnt-1))，
	// 命中末条后 action_time >= eTick.action_time，必须回退一格。
	vector<WTSOrdQueStruct> que;
	que.push_back(noisy_ordque(kT, kPrev, 200000000, 31));
	que.push_back(noisy_ordque(kT, kPrev, 210000000, 34));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/610901.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));

	vector<WTSTransStruct> trans;
	trans.push_back(noisy_trans(kT, kPrev, 200000000, 41));
	trans.push_back(noisy_trans(kT, kPrev, 210000000, 44));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/630901.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans))));

	WTSOrdQueSlice* q = _reader.readOrdQueSlice("SSE.STK.610901", 10, 20240109150000000ull);
	ASSERT_TRUE(q != NULL);
	ASSERT_EQ(q->size(), 1u);
	EXPECT_EQ(q->at(0)->action_date, kPrev);
	EXPECT_EQ(q->at(0)->action_time, 200000000u);
	q->release();

	WTSTransSlice* t = _reader.readTransSlice("SSE.STK.630901", 10, 20240109150000000ull);
	ASSERT_TRUE(t != NULL);
	ASSERT_EQ(t->size(), 1u);
	EXPECT_EQ(t->at(0)->action_date, kPrev);
	EXPECT_EQ(t->at(0)->action_time, 200000000u);
	t->release();
}

// WtDataReader.cpp: readTickSlice L414-429（长度校验后第二次访问空块）
TEST_F(R142ReaderHftHisTest, R142ReaderEmptyHisTickBlockSecondCall)
{
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/600902.dsb", string()));

	// 第一次：文件短于 HisTickBlock，清空缓存并返回；第二次：_block 为空早退。
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600902", 10, 20240109150000000ull) == NULL);
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600902", 10, 20240109150000000ull) == NULL);
}

} // namespace r142
