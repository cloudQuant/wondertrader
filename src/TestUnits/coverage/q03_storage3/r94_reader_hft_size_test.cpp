// r94 / WtDataReader：历史逐笔委托/成交文件的长度校验失败分支
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - readOrdDtlSlice 短文件 -> 清空并返回 NULL        :715-720
//  - readOrdDtlSlice 长度与块头 _size 不符 -> NULL    :724-728
//  - readOrdQueSlice 短文件 -> 清空并返回 NULL        :558-563（同类入口）
//  - readTransSlice  短文件 -> 清空并返回 NULL        :872-877
//
// 每个文件用不同交易日，避开 readOrdDtl/readTrans 共用 _his_ordque_map 查键的既有缺陷
// （BUG-R74-001）：只要队列缓存里没有同 key，三组文件都会各自进入装载分支。
// init(cfg) 只赋值目录，不起线程。
#include "r52_storage_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace r94 {

using namespace q03;
using std::string;
using std::vector;

class R94ReaderHftSizeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r94hftsz"));
		_root = _sandbox->root_abs();
		const string cfgfile = _root + "reader.json";
		ASSERT_TRUE(write_bytes(cfgfile, fmtutil::format("{{\"path\":\"{}\"}}", _root)));
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
	r52::BDMgr			_bd;
	HotMgr				_hot;
	ReaderSink			_sink{ &_bd, &_hot };
	WtDataReader			_reader;
};

// WtDataReader.cpp: L558-563 / L715-720 / L724-728 / L872-877
TEST_F(R94ReaderHftSizeTest, R94ReaderRejectsShortAndMismatchedHisHftFiles)
{
	// 委托队列：8 字节短文件
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240105/600000.dsb", string(8, 'Q')));
	uint32_t err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.600000", 2, 20240105150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// 委托队列：正常 CMP_V2 块尾多 1 字节 -> 长度校验失败
	vector<WTSOrdQueStruct> ques;
	ques.push_back(make_ordque(20240106, 20240106, 93000000, 1));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240106/600000.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(ques)) + "X"));
	err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.600000", 2, 20240106150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// 逐笔委托：8 字节短文件
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240107/600000.dsb", string(8, 'O')));
	err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readOrdDtlSlice("SSE.STK.600000", 2, 20240107150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// 逐笔委托：正常 CMP_V2 块尾多 1 字节
	vector<WTSOrdDtlStruct> dtls;
	dtls.push_back(make_orddtl(20240109, 20240109, 93000000, 3));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600000.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtls)) + "X"));
	err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readOrdDtlSlice("SSE.STK.600000", 2, 20240109150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// 逐笔成交：8 字节短文件
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240108/600000.dsb", string(8, 'T')));
	err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readTransSlice("SSE.STK.600000", 2, 20240108150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);
}

} // namespace r94
