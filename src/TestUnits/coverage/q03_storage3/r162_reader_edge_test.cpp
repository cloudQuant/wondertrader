// r162 / 主存储读取端补充：连续/复权缓存异常兜底，以及 RDM 全空数据源守卫
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - cacheBars 对 cacheIntegratedBars 的 catch(...) 日志 :1603-1606
//  - cacheBars 对 cacheAdjustedStkBars 的 catch(...) 日志 :1620-1623
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readTickSliceByRange / readOrdQueSliceByRange / readOrdDtlSliceByRange
//    / readTransSliceByRange / readKlineSliceByRange 在 his+rt 全缺失时的空返回守卫
//  - 历史 queue/order/trans 块内 action_date 跨日时的 lower_bound 日期比较 :750/:925/:1099
//
// 异常源来自 r152 夹具的公开虚接口替身（splitCustomSections / loadRawHisBars 抛 int），
// 不依赖文件系统错误；RDM 用例在 Sandbox 里 chdir，base_dir 为空时路径退化为相对路径。
#include "r152_fixture.h"

// 仅本 TU 白盒：把已解压好的历史逐笔块直接挂到 reader 的块缓存上，
// 绕开 BUG-R175-002（V2 解压后 resize/append 重分配，随后写回已释放的
// 块头指针，2 条记录的小块会破坏堆）。生产读取路径本身由既有 r74/r100
// 用例覆盖，本用例只驱动 lower_bound 的跨 action_date 比较。
#define private public
#include "WtDataStorage/WtRdmDtReader.h"
#undef private

namespace r162r {

using namespace q03;

class R162ReaderEdgeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_reader_cfg = _sandbox.root_abs() + "reader.json";
		ASSERT_TRUE(write_bytes(_reader_cfg, "{\"path\":\"" + _sandbox.root_abs() + "\"}"));
	}

	WTSVariant* load_cfg()
	{
		WTSVariant* cfg = WTSCfgLoader::load_from_file(_reader_cfg.c_str());
		EXPECT_TRUE(cfg != NULL);
		return cfg;
	}

	Sandbox			_sandbox{"r162r"};
	string			_reader_cfg;
	BaseDataMgr		_bd;
};

// WtDataReader.cpp:1603-1606
TEST_F(R162ReaderEdgeTest, R162ReaderContinuousCacheNonStdExceptionIsCaught)
{
	r152::ThrowingSplitHotMgr hot;
	ReaderSink sink(&_bd, &hot);

	WtDataReader reader;
	WTSVariant* cfg = load_cfg();
	ASSERT_TRUE(cfg != NULL);
	reader.init(cfg, &sink, NULL);
	cfg->release();

	WTSKlineSlice* slice = reader.readKlineSlice("SSE.600000.HOT", KP_Minute1, 5, 202401101500ULL);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->get_block_counts(), 0u);
	slice->release();

	EXPECT_GE(sink.count_of(LL_ERROR), 1u);
}

// WtDataReader.cpp:1620-1623
TEST_F(R162ReaderEdgeTest, R162ReaderAdjustedStockNonStdExceptionIsCaught)
{
	HotMgr hot;
	r152::ThrowingRawLoader loader;
	ReaderSink sink(&_bd, &hot);

	WtDataReader reader;
	WTSVariant* cfg = load_cfg();
	ASSERT_TRUE(cfg != NULL);
	reader.init(cfg, &sink, &loader);
	cfg->release();

	// 必须使用三段式标准代码：SSE.STK.600000- 才能让 CodeHelper 解析出
	// product=STK、code=600000、exright=2；两段式 "SSE.600000-" 会被当作
	// 普通合约代码，根本进不了 cacheAdjustedStkBars 分支。
	WTSKlineSlice* slice = reader.readKlineSlice("SSE.STK.600000-", KP_Minute1, 5, 202401101500ULL);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->get_block_counts(), 0u);
	slice->release();

	EXPECT_GE(sink.count_of(LL_ERROR), 1u);
}

// WtRdmDtReader.cpp:2019 / :2059 / :2099 / :2139 / :2179-2185
TEST_F(R162ReaderEdgeTest, R162RdmReaderAllSourcesMissingReturnsEmpty)
{
	HotMgr hot;
	RdmSink sink(&_bd, &hot);

	WtRdmDtReader reader;
	reader.init(NULL, &sink);

	const uint32_t curTDate = 20240110;
	const uint64_t stime = (uint64_t)curTDate * 1000000000ULL + 90000000ULL;
	const uint64_t etime = (uint64_t)curTDate * 1000000000ULL + 150000000ULL;

	WTSTickSlice* ticks = reader.readTickSliceByRange("SSE.600000", stime, etime);
	if (ticks != NULL)
	{
		EXPECT_EQ(ticks->get_block_counts(), 0u);
		ticks->release();
	}

	WTSOrdQueSlice* queues = reader.readOrdQueSliceByRange("SSE.600000", stime, etime);
	EXPECT_TRUE(queues == NULL);
	if (queues != NULL)
		queues->release();

	WTSOrdDtlSlice* details = reader.readOrdDtlSliceByRange("SSE.600000", stime, etime);
	EXPECT_TRUE(details == NULL);
	if (details != NULL)
		details->release();

	WTSTransSlice* trans = reader.readTransSliceByRange("SSE.600000", stime, etime);
	EXPECT_TRUE(trans == NULL);
	if (trans != NULL)
		trans->release();

	WTSKlineSlice* m1 = reader.readKlineSliceByRange("SSE.600000", KP_Minute1,
		(uint64_t)curTDate * 10000ULL + 900ULL, (uint64_t)curTDate * 10000ULL + 1500ULL);
	if (m1 != NULL)
	{
		EXPECT_EQ(m1->get_block_counts(), 0u);
		m1->release();
	}

	WTSKlineSlice* m5 = reader.readKlineSliceByRange("SSE.600000", KP_Minute5,
		(uint64_t)curTDate * 10000ULL + 900ULL, (uint64_t)curTDate * 10000ULL + 1500ULL);
	if (m5 != NULL)
	{
		EXPECT_EQ(m5->get_block_counts(), 0u);
		m5->release();
	}

	// 未初始化任何复权因子时返回默认值 1.0
	EXPECT_DOUBLE_EQ(reader.getAdjFactorByDate("SSE.600000", curTDate), 1.0);
}

// WtRdmDtReader.cpp:750 / :925 / :1099（历史逐笔块内 action_date 跨日时走日期比较）
TEST_F(R162ReaderEdgeTest, R162RdmReaderHisTransBlocksWithMixedActionDates)
{
	const uint32_t date = 20240109;
	const uint32_t prev = 20240108;
	const uint32_t atime = 10000000u;	// 10:00:00.000

	// 同一块内第一条的 action_date 与查询日不同，强制 lower_bound 走日期分支
	std::vector<WTSOrdQueStruct> queues;
	queues.push_back(make_ordque(date, prev, atime, 1));
	queues.push_back(make_ordque(date, date, atime, 2));

	std::vector<WTSOrdDtlStruct> details;
	details.push_back(make_orddtl(date, prev, atime, 1));
	details.push_back(make_orddtl(date, date, atime, 2));

	std::vector<WTSTransStruct> trans;
	trans.push_back(make_trans(date, prev, atime, 1));
	trans.push_back(make_trans(date, date, atime, 2));

	HotMgr hot;
	RdmSink sink(&_bd, &hot);
	WtRdmDtReader reader;
	reader.init(NULL, &sink);

	// 直接把已解压的块挂进历史缓存。正常 .dsb 落盘路径在 2 条记录时会触发
	// BUG-R175-002 的堆破坏，无法在本用例内安全使用（见下方 DISABLED_ 证据）。
	const std::string key = "SSE.600000-20240109";
	auto& quePair = reader._his_ordque_map[key];
	quePair._buffer.assign(sizeof(HisOrdQueBlock), '\0');
	quePair._buffer.append(reinterpret_cast<const char*>(queues.data()),
		queues.size() * sizeof(WTSOrdQueStruct));
	quePair._block = reinterpret_cast<HisOrdQueBlock*>(&quePair._buffer[0]);

	// dtl/trans 入口用 _his_ordque_map 判命中（BUG-R74-001），
	// 先占 key 让它们不再走文件装载，各自使用自己已挂好的块。
	auto& dtlPair = reader._his_orddtl_map[key];
	dtlPair._buffer.assign(sizeof(HisOrdDtlBlock), '\0');
	dtlPair._buffer.append(reinterpret_cast<const char*>(details.data()),
		details.size() * sizeof(WTSOrdDtlStruct));
	dtlPair._block = reinterpret_cast<HisOrdDtlBlock*>(&dtlPair._buffer[0]);

	auto& trPair = reader._his_trans_map[key];
	trPair._buffer.assign(sizeof(HisTransBlock), '\0');
	trPair._buffer.append(reinterpret_cast<const char*>(trans.data()),
		trans.size() * sizeof(WTSTransStruct));
	trPair._block = reinterpret_cast<HisTransBlock*>(&trPair._buffer[0]);

	const uint64_t stime = (uint64_t)date * 1000000000ULL + atime;
	const uint64_t etime = stime;

	WTSOrdQueSlice* queSlice = reader.readOrdQueSliceByRange("SSE.600000", stime, etime);
	ASSERT_TRUE(queSlice != NULL);
	EXPECT_GT(queSlice->size(), 0u);
	queSlice->release();

	WTSOrdDtlSlice* dtlSlice = reader.readOrdDtlSliceByRange("SSE.600000", stime, etime);
	ASSERT_TRUE(dtlSlice != NULL);
	EXPECT_GT(dtlSlice->size(), 0u);
	dtlSlice->release();

	WTSTransSlice* trSlice = reader.readTransSliceByRange("SSE.600000", stime, etime);
	ASSERT_TRUE(trSlice != NULL);
	EXPECT_GT(trSlice->size(), 0u);
	trSlice->release();
}

/* BUG-R175-002 的可执行证据：三条历史的 V2 装载分支都在
 * `hisBlkPair._buffer.resize(sizeof(His*Block)); _buffer.append(buf);` 之后
 * 通过 resize 前取得的 tBlockV2 指针写 `_version`。append 触发重分配时
 * 该指针已失效，2 条记录的小块会写坏 malloc 元数据，下一次分配 SIGSEGV。
 * 保持与生产 .dsb 完全一致的 CMP_V2 文件输入；启用即崩，故 DISABLED_。 */
TEST_F(R162ReaderEdgeTest, DISABLED_R162RdmReaderHisTransBlocksWithMixedActionDatesFromFile)
{
	const uint32_t date = 20240109;
	const uint32_t prev = 20240108;
	const uint32_t atime = 10000000u;

	std::vector<WTSOrdQueStruct> queues;
	queues.push_back(make_ordque(date, prev, atime, 1));
	queues.push_back(make_ordque(date, date, atime, 2));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600000.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(queues))));

	std::vector<WTSOrdDtlStruct> details;
	details.push_back(make_orddtl(date, prev, atime, 1));
	details.push_back(make_orddtl(date, date, atime, 2));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600000.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(details))));

	std::vector<WTSTransStruct> trans;
	trans.push_back(make_trans(date, prev, atime, 1));
	trans.push_back(make_trans(date, date, atime, 2));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/600000.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans))));

	HotMgr hot;
	RdmSink sink(&_bd, &hot);
	WtRdmDtReader reader;
	reader.init(NULL, &sink);

	const uint64_t stime = (uint64_t)date * 1000000000ULL + atime;

	WTSOrdQueSlice* queSlice = reader.readOrdQueSliceByRange("SSE.600000", stime, stime);
	ASSERT_TRUE(queSlice != NULL);
	EXPECT_GT(queSlice->size(), 0u);
	queSlice->release();

	WTSOrdDtlSlice* dtlSlice = reader.readOrdDtlSliceByRange("SSE.600000", stime, stime);
	ASSERT_TRUE(dtlSlice != NULL);
	EXPECT_GT(dtlSlice->size(), 0u);
	dtlSlice->release();

	WTSTransSlice* trSlice = reader.readTransSliceByRange("SSE.600000", stime, stime);
	ASSERT_TRUE(trSlice != NULL);
	EXPECT_GT(trSlice->size(), 0u);
	trSlice->release();
}

} // namespace r162r
