// r142 / WtRdmDtReader：历史逐笔比较器的跨 action_date 分支、缺失/短文件守卫、
//       历史 K 线右界回退
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readOrdQueSliceByRange  历史 lambda 日期分支 :750 / :774
//  - readOrdDtlSliceByRange  历史 lambda 日期分支 :925 / :948
//  - readTransSliceByRange   历史 lambda 日期分支 :1099 / :1122
//  - readOrdDtlSliceByRange  缺失 RT 文件 :831；缺失历史文件 :883
//  - readTransSliceByRange   缺失 RT 文件 :1005；缺失历史文件 :1057
//  - readOrdQueSliceByRange  空历史块第二次访问 :739-740
//  - indexBarFromCacheByRange 历史右界落在两根之间回退 :1760-1766
//
// 历史逐笔载荷用 r142_fixture.h 的不可压缩样本，避开 BUG-R132-001 同型的
// 「resize+append 后旧头部悬垂写」；RT 缺失路径不读文件，短文件在长度校验处
// 直接返回。全部在 q03::Sandbox 临时目录，init(NULL) 不起 5 秒巡检线程。
#include "r142_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace r142 {

using namespace q03;
using std::string;
using std::vector;

bool records_equal(const WTSOrdQueStruct& actual, const WTSOrdQueStruct& expected)
{
	return memcmp(actual.exchg, expected.exchg, sizeof(actual.exchg)) == 0
		&& memcmp(actual.code, expected.code, sizeof(actual.code)) == 0
		&& actual.trading_date == expected.trading_date
		&& actual.action_date == expected.action_date
		&& actual.action_time == expected.action_time
		&& actual.side == expected.side
		&& actual.price == expected.price
		&& actual.order_items == expected.order_items
		&& actual.qsize == expected.qsize
		&& memcmp(actual.volumes, expected.volumes, sizeof(actual.volumes)) == 0;
}

bool records_equal(const WTSOrdDtlStruct& actual, const WTSOrdDtlStruct& expected)
{
	return memcmp(actual.exchg, expected.exchg, sizeof(actual.exchg)) == 0
		&& memcmp(actual.code, expected.code, sizeof(actual.code)) == 0
		&& actual.trading_date == expected.trading_date
		&& actual.action_date == expected.action_date
		&& actual.action_time == expected.action_time
		&& actual.index == expected.index
		&& actual.price == expected.price
		&& actual.volume == expected.volume
		&& actual.side == expected.side
		&& actual.otype == expected.otype
		&& actual.orderid == expected.orderid;
}

bool records_equal(const WTSTransStruct& actual, const WTSTransStruct& expected)
{
	return memcmp(actual.exchg, expected.exchg, sizeof(actual.exchg)) == 0
		&& memcmp(actual.code, expected.code, sizeof(actual.code)) == 0
		&& actual.trading_date == expected.trading_date
		&& actual.action_date == expected.action_date
		&& actual.action_time == expected.action_time
		&& actual.index == expected.index
		&& actual.ttype == expected.ttype
		&& actual.side == expected.side
		&& actual.price == expected.price
		&& actual.volume == expected.volume
		&& actual.askorder == expected.askorder
		&& actual.bidorder == expected.bidorder;
}

template<typename T, typename Slice>
bool slice_matches_records(Slice* slice, const vector<T>& expected)
{
	if (slice == NULL || slice->size() != expected.size())
		return false;

	for (std::size_t i = 0; i < expected.size(); i++)
	{
		if (slice->at(i) == NULL || !records_equal(*slice->at(i), expected[i]))
			return false;
	}

	return true;
}

string make_cmp_v2_block_with_compressed_payload(uint16_t block_type, const string& compressed)
{
	BlockHeaderV2 head;
	memset(&head, 0, sizeof(head));
	memcpy(head._blk_flag, BLK_FLAG, FLAG_SIZE);
	head._type = block_type;
	head._version = BLOCK_VERSION_CMP_V2;
	head._size = compressed.size();

	string block((const char*)&head, sizeof(head));
	block.append(compressed);
	return block;
}

string make_cmp_v2_block_with_declared_size(uint16_t block_type, const string& payload,
	uint64_t declared_size)
{
	string block = make_block(block_type, BLOCK_VERSION_CMP_V2, payload);
	BlockHeaderV2 head;
	memcpy(&head, block.data(), sizeof(head));
	head._size = declared_size;
	memcpy(block.data(), &head, sizeof(head));
	return block;
}

string make_cmp_v2_block_with_small_size_mismatch(uint16_t block_type, const string& payload)
{
	string block = make_block(block_type, BLOCK_VERSION_CMP_V2, payload);
	BlockHeaderV2 head;
	memcpy(&head, block.data(), sizeof(head));
	head._size++;
	memcpy(block.data(), &head, sizeof(head));
	return block;
}

template<typename T, typename Slice, typename ReadFn>
void expect_malformed_cmp_v2_rejected_and_retry(RdmSink& sink, uint16_t block_type,
	const string& path, const string& malformed_block, const char* error_text,
	const T& valid_record, ReadFn read_slice)
{
	ASSERT_TRUE(write_bytes(path, malformed_block));
	const uint32_t errors_before = sink.count_of(LL_ERROR);
	Slice* rejected = NULL;
	EXPECT_NO_THROW(rejected = read_slice());
	EXPECT_EQ(nullptr, rejected);
	EXPECT_EQ(errors_before + 1, sink.count_of(LL_ERROR));
	EXPECT_NE(string::npos, sink.last_log().find(error_text));

	// The failed load must not poison this key: replacing the file and reading
	// through the same reader/key should load and expose the valid record.
	const vector<T> expected = { valid_record };
	ASSERT_TRUE(write_bytes(path,
		make_block(block_type, BLOCK_VERSION_CMP_V2, pack_items(expected))));
	Slice* retried = NULL;
	EXPECT_NO_THROW(retried = read_slice());
	ASSERT_NE(nullptr, retried);
	EXPECT_TRUE(slice_matches_records(retried, expected));
	retried->release();
	EXPECT_EQ(errors_before + 1, sink.count_of(LL_ERROR));
}

class ReaderTransformFailureScope
{
public:
	ReaderTransformFailureScope(uint16_t block_type,
		wtp::RdmDtReaderTransformFailureStage stage)
	{
		wtp::setRdmDtReaderTransformFailure(block_type, stage);
	}
	~ReaderTransformFailureScope()
	{
		wtp::setRdmDtReaderTransformFailure(0,
			wtp::RdmDtReaderTransformFailureStage::None);
	}
};

template<typename T, typename Slice, typename ReadFn>
void expect_transform_failure_and_retry(RdmSink& sink, uint16_t block_type,
	wtp::RdmDtReaderTransformFailureStage stage, const string& path,
	const T& valid_record, ReadFn read_slice)
{
	const vector<T> expected = { valid_record };
	ASSERT_TRUE(write_bytes(path,
		make_block(block_type, BLOCK_VERSION_CMP_V2, pack_items(expected))));
	const uint32_t errors_before = sink.count_of(LL_ERROR);
	Slice* rejected = NULL;
	{
		ReaderTransformFailureScope failure_scope(block_type, stage);
		EXPECT_NO_THROW(rejected = read_slice());
	}
	EXPECT_EQ(nullptr, rejected);
	EXPECT_EQ(errors_before + 1, sink.count_of(LL_ERROR));
	EXPECT_NE(string::npos, sink.last_log().find("Rebuilding"));
	EXPECT_NE(string::npos, sink.last_log().find("injected RDM block transform failure"));

	// Replacing and rereading the same key proves a failed rebuild did not leave
	// a partial/raw-version buffer cached in the reader.
	ASSERT_TRUE(write_bytes(path,
		make_block(block_type, BLOCK_VERSION_CMP_V2, pack_items(expected))));
	Slice* retried = NULL;
	EXPECT_NO_THROW(retried = read_slice());
	ASSERT_NE(nullptr, retried);
	EXPECT_TRUE(slice_matches_records(retried, expected));
	retried->release();
	EXPECT_EQ(errors_before + 1, sink.count_of(LL_ERROR));
}

struct ReaderBufferRebuildObservation
{
	uint16_t block_type;
	uintptr_t old_data_address;
	uintptr_t new_data_address;
	std::size_t old_capacity;
	std::size_t new_capacity;
	std::size_t rebuilt_size;
	uint16_t rebuilt_version;
};

std::vector<ReaderBufferRebuildObservation> g_reader_buffer_rebuild_observations;

void capture_reader_buffer_rebuild(uint16_t block_type, uintptr_t old_data_address,
	uintptr_t new_data_address, std::size_t old_capacity, std::size_t new_capacity,
	std::size_t rebuilt_size, uint16_t rebuilt_version)
{
	g_reader_buffer_rebuild_observations.push_back({ block_type, old_data_address,
		new_data_address, old_capacity, new_capacity, rebuilt_size, rebuilt_version });
}

class ReaderBufferRebuildProbeScope
{
public:
	ReaderBufferRebuildProbeScope()
	{
		g_reader_buffer_rebuild_observations.clear();
		wtp::setRdmDtReaderBufferRebuildHook(&capture_reader_buffer_rebuild);
	}
	~ReaderBufferRebuildProbeScope()
	{
		wtp::setRdmDtReaderBufferRebuildHook(nullptr);
	}
};

void expect_reader_buffer_rebuild(uint16_t block_type, bool expected_reallocation,
	std::size_t expected_size)
{
	const auto found = std::find_if(g_reader_buffer_rebuild_observations.begin(),
		g_reader_buffer_rebuild_observations.end(), [block_type](const ReaderBufferRebuildObservation& item) {
			return item.block_type == block_type;
		});
	ASSERT_NE(g_reader_buffer_rebuild_observations.end(), found) << "block type " << block_type;
	EXPECT_EQ(std::count_if(g_reader_buffer_rebuild_observations.begin(),
		g_reader_buffer_rebuild_observations.end(), [block_type](const ReaderBufferRebuildObservation& item) {
			return item.block_type == block_type;
		}), 1);
	EXPECT_EQ(found->rebuilt_size, expected_size);
	EXPECT_GE(found->new_capacity, expected_size);
	EXPECT_EQ(found->old_data_address != found->new_data_address, expected_reallocation);
	EXPECT_EQ(found->old_capacity < expected_size, expected_reallocation);
	if (!expected_reallocation)
		EXPECT_EQ(found->new_capacity, found->old_capacity);
	EXPECT_EQ(found->rebuilt_version, BLOCK_VERSION_RAW);

	std::ostringstream value;
	value << "old_addr=0x" << std::hex << found->old_data_address
		<< ",new_addr=0x" << found->new_data_address << std::dec
		<< ",old_capacity=" << found->old_capacity
		<< ",new_capacity=" << found->new_capacity
		<< ",rebuilt_size=" << found->rebuilt_size
		<< ",version=" << found->rebuilt_version;
	::testing::Test::RecordProperty("r175_cmp_v2_buffer_" + std::to_string(block_type), value.str());
}

void fill_cstring_suffix_noise(char* value, std::size_t capacity, uint32_t seed)
{
	std::size_t terminator = 0;
	while (terminator < capacity && value[terminator] != '\0')
		terminator++;
	if (terminator == capacity)
		return;

	// Preserve the C-string value and terminator; bytes after the first NUL are
	// outside the field value and make the otherwise-valid CMP_V2 frame incompressible.
	for (std::size_t i = terminator + 1; i < capacity; i++)
	{
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		value[i] = (char)(seed >> 24);
	}
}

class R142RdmHftHisTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r142rdm"));
		_bd.set_cur_tdate(20240110);
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr			_bd;
	HotMgr				_hot;
	RdmSink				_sink{ &_bd, &_hot };
	WtRdmDtReader			_reader;
};

// WtRdmDtReader.cpp: readOrdQueSliceByRange L748-782（历史比较器 L750 / L774）
TEST_F(R142RdmHftHisTest, R142RdmHisQueueCrossActionDateByRange)
{
	const uint32_t kT = 20240109;
	const uint32_t kPrev = 20240108;

	vector<WTSOrdQueStruct> items;
	items.push_back(noisy_ordque(kT, kPrev, 210000000, 1));	// 前一日夜盘
	items.push_back(noisy_ordque(kT, kT, 93000000, 4));
	items.push_back(noisy_ordque(kT, kT, 103000000, 7));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/610801.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(items))));

	// 同一交易日窗口 [09:00, 11:00]：eTick/sTick 的 action_date == 0109，
	// 块内首条 action_date == 0108，两个 lower_bound 都命中日期分支。
	WTSOrdQueSlice* s = _reader.readOrdQueSliceByRange("SSE.STK.610801",
		20240109090000000ull, 20240109110000000ull);
	ASSERT_TRUE(s != NULL);
	ASSERT_EQ(s->size(), 2u);
	EXPECT_EQ(s->at(0)->action_date, kT);
	EXPECT_EQ(s->at(0)->action_time, 93000000u);
	EXPECT_EQ(s->at(1)->action_time, 103000000u);
	s->release();
}

// WtRdmDtReader.cpp: readOrdDtlSliceByRange L923-948 / readTransSliceByRange L1097-1122
TEST_F(R142RdmHftHisTest, R142RdmHisDetailTransCrossActionDateByRange)
{
	const uint32_t kT = 20240109;
	const uint32_t kPrev = 20240108;

	vector<WTSOrdDtlStruct> dtls;
	dtls.push_back(noisy_orddtl(kT, kPrev, 210000000, 11));
	dtls.push_back(noisy_orddtl(kT, kT, 93000000, 14));
	dtls.push_back(noisy_orddtl(kT, kT, 103000000, 17));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/620801.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtls))));

	vector<WTSTransStruct> trans;
	trans.push_back(noisy_trans(kT, kPrev, 210000000, 21));
	trans.push_back(noisy_trans(kT, kT, 93000000, 24));
	trans.push_back(noisy_trans(kT, kT, 103000000, 27));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/630801.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans))));

	WTSOrdDtlSlice* d = _reader.readOrdDtlSliceByRange("SSE.STK.620801",
		20240109090000000ull, 20240109110000000ull);
	ASSERT_TRUE(d != NULL);
	ASSERT_EQ(d->size(), 2u);
	EXPECT_EQ(d->at(0)->action_date, kT);
	EXPECT_EQ(d->at(0)->action_time, 93000000u);
	d->release();

	WTSTransSlice* t = _reader.readTransSliceByRange("SSE.STK.630801",
		20240109090000000ull, 20240109110000000ull);
	ASSERT_TRUE(t != NULL);
	ASSERT_EQ(t->size(), 2u);
	EXPECT_EQ(t->at(0)->action_date, kT);
	EXPECT_EQ(t->at(0)->action_time, 93000000u);
	t->release();
}

TEST_F(R142RdmHftHisTest, R142RdmCompressedExpansionPreservesAllFieldsAndCacheReads)
{
	const uint32_t kTradingDate = 20240109;
	const uint32_t kRecordCount = 1024;
	const uint64_t kStart = 20240109090000000ull;
	const uint64_t kEnd = 20240109100000000ull;
	const char* queue_code = "610805";
	const char* detail_code = "620805";
	const char* trans_code = "630805";
	const string queue_path = "his/queue/SSE/20240109/610805.dsb";
	const string detail_path = "his/orders/SSE/20240109/620805.dsb";
	const string trans_path = "his/trans/SSE/20240109/630805.dsb";

	vector<WTSOrdQueStruct> queues;
	vector<WTSOrdDtlStruct> details;
	vector<WTSTransStruct> trans;
	queues.reserve(kRecordCount);
	details.reserve(kRecordCount);
	trans.reserve(kRecordCount);
	for (uint32_t i = 0; i < kRecordCount; i++)
	{
		const uint32_t action_time = 93000000 + i * 1000;

		WTSOrdQueStruct queue;
		snprintf(queue.exchg, sizeof(queue.exchg), "%s", "SSE");
		snprintf(queue.code, sizeof(queue.code), "%s", queue_code);
		queue.trading_date = kTradingDate;
		queue.action_date = kTradingDate;
		queue.action_time = action_time;
		queue.side = i % 2 == 0 ? BDT_Buy : BDT_Sell;
		queue.price = 19.5;
		queue.order_items = 2;
		queue.qsize = 2;
		queue.volumes[0] = i + 1;
		queue.volumes[1] = 2;
		queues.push_back(queue);

		WTSOrdDtlStruct detail;
		snprintf(detail.exchg, sizeof(detail.exchg), "%s", "SSE");
		snprintf(detail.code, sizeof(detail.code), "%s", detail_code);
		detail.trading_date = kTradingDate;
		detail.action_date = kTradingDate;
		detail.action_time = action_time;
		detail.index = 100000 + i;
		detail.price = 29.25;
		detail.volume = i + 1;
		detail.side = i % 2 == 0 ? BDT_Buy : BDT_Sell;
		detail.otype = i % 2 == 0 ? ODT_Append : ODT_LimitPrice;
		detail.orderid = 200000 + i;
		details.push_back(detail);

		WTSTransStruct tran;
		snprintf(tran.exchg, sizeof(tran.exchg), "%s", "SSE");
		snprintf(tran.code, sizeof(tran.code), "%s", trans_code);
		tran.trading_date = kTradingDate;
		tran.action_date = kTradingDate;
		tran.action_time = action_time;
		tran.index = 300000 + i;
		tran.ttype = i % 2 == 0 ? TT_Match : TT_Cancel;
		tran.side = i % 2 == 0 ? BDT_Buy : BDT_Sell;
		tran.price = 39.75;
		tran.volume = i + 1;
		tran.askorder = 400000 + i;
		tran.bidorder = 500000 + i;
		trans.push_back(tran);
	}

	const string queue_payload = pack_items(queues);
	const string detail_payload = pack_items(details);
	const string trans_payload = pack_items(trans);
	ASSERT_TRUE(write_bytes(queue_path,
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, queue_payload)));
	ASSERT_TRUE(write_bytes(detail_path,
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, detail_payload)));
	ASSERT_TRUE(write_bytes(trans_path,
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, trans_payload)));

	const string queue_std_code = fmtutil::format("SSE.STK.{}", queue_code);
	const string detail_std_code = fmtutil::format("SSE.STK.{}", detail_code);
	const string trans_std_code = fmtutil::format("SSE.STK.{}", trans_code);
	const auto check_slices = [&](const char* stage)
	{
		WTSOrdQueSlice* queue_slice = _reader.readOrdQueSliceByRange(
			queue_std_code.c_str(), kStart, kEnd);
		ASSERT_NE(nullptr, queue_slice) << stage << " queue";
		EXPECT_TRUE(slice_matches_records(queue_slice, queues)) << stage << " queue fields";
		queue_slice->release();

		WTSOrdDtlSlice* detail_slice = _reader.readOrdDtlSliceByRange(
			detail_std_code.c_str(), kStart, kEnd);
		ASSERT_NE(nullptr, detail_slice) << stage << " detail";
		EXPECT_TRUE(slice_matches_records(detail_slice, details)) << stage << " detail fields";
		detail_slice->release();

		WTSTransSlice* trans_slice = _reader.readTransSliceByRange(
			trans_std_code.c_str(), kStart, kEnd);
		ASSERT_NE(nullptr, trans_slice) << stage << " trans";
		EXPECT_TRUE(slice_matches_records(trans_slice, trans)) << stage << " trans fields";
		trans_slice->release();
	};

	ReaderBufferRebuildProbeScope probe_scope;
	check_slices("initial load");
	ASSERT_EQ(g_reader_buffer_rebuild_observations.size(), 3u);
	expect_reader_buffer_rebuild(BT_HIS_OrdQueue, true,
		sizeof(HisOrdQueBlock) + queue_payload.size());
	expect_reader_buffer_rebuild(BT_HIS_OrdDetail, true,
		sizeof(HisOrdDtlBlock) + detail_payload.size());
	expect_reader_buffer_rebuild(BT_HIS_Trnsctn, true,
		sizeof(HisTransBlock) + trans_payload.size());
	ASSERT_TRUE(std::filesystem::remove(queue_path));
	ASSERT_TRUE(std::filesystem::remove(detail_path));
	ASSERT_TRUE(std::filesystem::remove(trans_path));
	check_slices("cached reread after file removal");
}

TEST_F(R142RdmHftHisTest, R175RdmCmpV2NoGrowthPreservesAllFields)
{
	const uint32_t kTradingDate = 20240109;
	const uint64_t kStart = 20240109090000000ull;
	const uint64_t kEnd = 20240109150000000ull;
	const char* queue_code = "610806";
	const char* detail_code = "620806";
	const char* trans_code = "630806";
	const string queue_path = "his/queue/SSE/20240109/610806.dsb";
	const string detail_path = "his/orders/SSE/20240109/620806.dsb";
	const string trans_path = "his/trans/SSE/20240109/630806.dsb";

	WTSOrdQueStruct queue = make_ordque(kTradingDate, kTradingDate, 93000000, 10806);
	fill_cstring_suffix_noise(queue.exchg, sizeof(queue.exchg), 0x12345678u);
	fill_cstring_suffix_noise(queue.code, sizeof(queue.code), 0x87654321u);
	queue.order_items = 50;
	queue.qsize = 50;
	for (uint32_t i = 0; i < 50; i++)
		queue.volumes[i] = (i * 2654435761u) ^ 0x9e3779b9u;
	const vector<WTSOrdQueStruct> queues = { queue };

	WTSOrdDtlStruct detail = make_orddtl(kTradingDate, kTradingDate, 93000000, 20806);
	fill_cstring_suffix_noise(detail.exchg, sizeof(detail.exchg), 0x23456789u);
	fill_cstring_suffix_noise(detail.code, sizeof(detail.code), 0x98765432u);
	detail.index = 0x9182736455463728ull;
	detail.price = 123.456;
	detail.volume = 0xa5f12345u;
	detail.side = BDT_Sell;
	detail.otype = ODT_AnyPrice;
	detail.orderid = 0x7fedcba987654321ll;
	const vector<WTSOrdDtlStruct> details = { detail };

	WTSTransStruct tran = make_trans(kTradingDate, kTradingDate, 93000000, 30806);
	fill_cstring_suffix_noise(tran.exchg, sizeof(tran.exchg), 0x3456789au);
	fill_cstring_suffix_noise(tran.code, sizeof(tran.code), 0xa9876543u);
	tran.index = 0xfedcba9876543210ull;
	tran.ttype = TT_Cancel;
	tran.side = BDT_Buy;
	tran.price = 321.987;
	tran.volume = 0xfedcba98u;
	tran.askorder = 0x7182736455463728ll;
	tran.bidorder = 0x7fedcba987654321ll;
	const vector<WTSTransStruct> trans = { tran };

	const string queue_payload = pack_items(queues);
	const string detail_payload = pack_items(details);
	const string trans_payload = pack_items(trans);
	ASSERT_TRUE(write_bytes(queue_path,
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, queue_payload)));
	ASSERT_TRUE(write_bytes(detail_path,
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, detail_payload)));
	ASSERT_TRUE(write_bytes(trans_path,
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, trans_payload)));

	const string queue_std_code = fmtutil::format("SSE.STK.{}", queue_code);
	const string detail_std_code = fmtutil::format("SSE.STK.{}", detail_code);
	const string trans_std_code = fmtutil::format("SSE.STK.{}", trans_code);
	ReaderBufferRebuildProbeScope probe_scope;
	WTSOrdQueSlice* queue_slice = _reader.readOrdQueSliceByRange(
		queue_std_code.c_str(), kStart, kEnd);
	ASSERT_NE(nullptr, queue_slice);
	EXPECT_TRUE(slice_matches_records(queue_slice, queues));
	queue_slice->release();

	WTSOrdDtlSlice* detail_slice = _reader.readOrdDtlSliceByRange(
		detail_std_code.c_str(), kStart, kEnd);
	ASSERT_NE(nullptr, detail_slice);
	EXPECT_TRUE(slice_matches_records(detail_slice, details));
	detail_slice->release();

	WTSTransSlice* trans_slice = _reader.readTransSliceByRange(
		trans_std_code.c_str(), kStart, kEnd);
	ASSERT_NE(nullptr, trans_slice);
	EXPECT_TRUE(slice_matches_records(trans_slice, trans));
	trans_slice->release();

	ASSERT_EQ(g_reader_buffer_rebuild_observations.size(), 3u);
	expect_reader_buffer_rebuild(BT_HIS_OrdQueue, false,
		sizeof(HisOrdQueBlock) + queue_payload.size());
	expect_reader_buffer_rebuild(BT_HIS_OrdDetail, false,
		sizeof(HisOrdDtlBlock) + detail_payload.size());
	expect_reader_buffer_rebuild(BT_HIS_Trnsctn, false,
		sizeof(HisTransBlock) + trans_payload.size());
}

TEST_F(R142RdmHftHisTest, R175RdmCmpV2RejectsMalformedPayloadsAndAllowsRetry)
{
	const uint32_t kTradingDate = 20240109;
	const uint64_t kStart = 20240109090000000ull;
	const uint64_t kEnd = 20240109100000000ull;

	WTSOrdQueStruct queue = make_ordque(kTradingDate, kTradingDate, 93000000, 619901);
	vector<WTSOrdQueStruct> queues = { queue };
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdQueStruct, WTSOrdQueSlice>(
		_sink, BT_HIS_OrdQueue, "his/queue/SSE/20240109/619900.dsb",
		make_cmp_v2_block_with_declared_size(BT_HIS_OrdQueue, pack_items(queues),
			std::numeric_limits<uint64_t>::max()),
		"Sizechecking", queue, [&]() {
			return _reader.readOrdQueSliceByRange("SSE.STK.619900", kStart, kEnd);
		});
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdQueStruct, WTSOrdQueSlice>(
		_sink, BT_HIS_OrdQueue, "his/queue/SSE/20240109/619899.dsb",
		make_cmp_v2_block_with_declared_size(BT_HIS_OrdQueue, pack_items(queues),
			std::numeric_limits<uint64_t>::max() - 1),
		"Sizechecking", queue, [&]() {
			return _reader.readOrdQueSliceByRange("SSE.STK.619899", kStart, kEnd);
		});
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdQueStruct, WTSOrdQueSlice>(
		_sink, BT_HIS_OrdQueue, "his/queue/SSE/20240109/619898.dsb",
		make_cmp_v2_block_with_small_size_mismatch(BT_HIS_OrdQueue, pack_items(queues)),
		"Sizechecking", queue, [&]() {
			return _reader.readOrdQueSliceByRange("SSE.STK.619898", kStart, kEnd);
		});

	string unaligned_queue = pack_items(queues);
	unaligned_queue.push_back('\x7f');
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdQueStruct, WTSOrdQueSlice>(
		_sink, BT_HIS_OrdQueue, "his/queue/SSE/20240109/619901.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, unaligned_queue),
		"incomplete record", queue, [&]() {
			return _reader.readOrdQueSliceByRange("SSE.STK.619901", kStart, kEnd);
		});

	WTSOrdQueStruct queue_zstd = make_ordque(kTradingDate, kTradingDate, 93000000, 619902);
	const string invalid_zstd("definitely-not-zstd");
	EXPECT_THROW(WTSCmpHelper::uncompress_data(invalid_zstd.data(), invalid_zstd.size()), std::exception);
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdQueStruct, WTSOrdQueSlice>(
		_sink, BT_HIS_OrdQueue, "his/queue/SSE/20240109/619902.dsb",
		make_cmp_v2_block_with_compressed_payload(BT_HIS_OrdQueue, invalid_zstd),
		"Uncompressing", queue_zstd, [&]() {
			return _reader.readOrdQueSliceByRange("SSE.STK.619902", kStart, kEnd);
		});

	expect_transform_failure_and_retry<WTSOrdQueStruct, WTSOrdQueSlice>(
		_sink, BT_HIS_OrdQueue, wtp::RdmDtReaderTransformFailureStage::BeforeResize,
		"his/queue/SSE/20240109/619910.dsb", queue, [&]() {
			return _reader.readOrdQueSliceByRange("SSE.STK.619910", kStart, kEnd);
		});
	expect_transform_failure_and_retry<WTSOrdQueStruct, WTSOrdQueSlice>(
		_sink, BT_HIS_OrdQueue, wtp::RdmDtReaderTransformFailureStage::BeforeAppend,
		"his/queue/SSE/20240109/619911.dsb", queue, [&]() {
			return _reader.readOrdQueSliceByRange("SSE.STK.619911", kStart, kEnd);
		});

	WTSOrdDtlStruct detail = make_orddtl(kTradingDate, kTradingDate, 93000000, 629901);
	vector<WTSOrdDtlStruct> details = { detail };
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdDtlStruct, WTSOrdDtlSlice>(
		_sink, BT_HIS_OrdDetail, "his/orders/SSE/20240109/629900.dsb",
		make_cmp_v2_block_with_declared_size(BT_HIS_OrdDetail, pack_items(details),
			std::numeric_limits<uint64_t>::max()),
		"Sizechecking", detail, [&]() {
			return _reader.readOrdDtlSliceByRange("SSE.STK.629900", kStart, kEnd);
		});
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdDtlStruct, WTSOrdDtlSlice>(
		_sink, BT_HIS_OrdDetail, "his/orders/SSE/20240109/629899.dsb",
		make_cmp_v2_block_with_declared_size(BT_HIS_OrdDetail, pack_items(details),
			std::numeric_limits<uint64_t>::max() - 1),
		"Sizechecking", detail, [&]() {
			return _reader.readOrdDtlSliceByRange("SSE.STK.629899", kStart, kEnd);
		});
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdDtlStruct, WTSOrdDtlSlice>(
		_sink, BT_HIS_OrdDetail, "his/orders/SSE/20240109/629898.dsb",
		make_cmp_v2_block_with_small_size_mismatch(BT_HIS_OrdDetail, pack_items(details)),
		"Sizechecking", detail, [&]() {
			return _reader.readOrdDtlSliceByRange("SSE.STK.629898", kStart, kEnd);
		});

	string unaligned_detail = pack_items(details);
	unaligned_detail.push_back('\x7f');
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdDtlStruct, WTSOrdDtlSlice>(
		_sink, BT_HIS_OrdDetail, "his/orders/SSE/20240109/629901.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, unaligned_detail),
		"incomplete record", detail, [&]() {
			return _reader.readOrdDtlSliceByRange("SSE.STK.629901", kStart, kEnd);
		});

	WTSOrdDtlStruct detail_zstd = make_orddtl(kTradingDate, kTradingDate, 93000000, 629902);
	expect_malformed_cmp_v2_rejected_and_retry<WTSOrdDtlStruct, WTSOrdDtlSlice>(
		_sink, BT_HIS_OrdDetail, "his/orders/SSE/20240109/629902.dsb",
		make_cmp_v2_block_with_compressed_payload(BT_HIS_OrdDetail, invalid_zstd),
		"Uncompressing", detail_zstd, [&]() {
			return _reader.readOrdDtlSliceByRange("SSE.STK.629902", kStart, kEnd);
		});

	expect_transform_failure_and_retry<WTSOrdDtlStruct, WTSOrdDtlSlice>(
		_sink, BT_HIS_OrdDetail, wtp::RdmDtReaderTransformFailureStage::BeforeResize,
		"his/orders/SSE/20240109/629910.dsb", detail, [&]() {
			return _reader.readOrdDtlSliceByRange("SSE.STK.629910", kStart, kEnd);
		});
	expect_transform_failure_and_retry<WTSOrdDtlStruct, WTSOrdDtlSlice>(
		_sink, BT_HIS_OrdDetail, wtp::RdmDtReaderTransformFailureStage::BeforeAppend,
		"his/orders/SSE/20240109/629911.dsb", detail, [&]() {
			return _reader.readOrdDtlSliceByRange("SSE.STK.629911", kStart, kEnd);
		});

	WTSTransStruct trans = make_trans(kTradingDate, kTradingDate, 93000000, 639901);
	vector<WTSTransStruct> transactions = { trans };
	expect_malformed_cmp_v2_rejected_and_retry<WTSTransStruct, WTSTransSlice>(
		_sink, BT_HIS_Trnsctn, "his/trans/SSE/20240109/639900.dsb",
		make_cmp_v2_block_with_declared_size(BT_HIS_Trnsctn, pack_items(transactions),
			std::numeric_limits<uint64_t>::max()),
		"Sizechecking", trans, [&]() {
			return _reader.readTransSliceByRange("SSE.STK.639900", kStart, kEnd);
		});
	expect_malformed_cmp_v2_rejected_and_retry<WTSTransStruct, WTSTransSlice>(
		_sink, BT_HIS_Trnsctn, "his/trans/SSE/20240109/639899.dsb",
		make_cmp_v2_block_with_declared_size(BT_HIS_Trnsctn, pack_items(transactions),
			std::numeric_limits<uint64_t>::max() - 1),
		"Sizechecking", trans, [&]() {
			return _reader.readTransSliceByRange("SSE.STK.639899", kStart, kEnd);
		});
	expect_malformed_cmp_v2_rejected_and_retry<WTSTransStruct, WTSTransSlice>(
		_sink, BT_HIS_Trnsctn, "his/trans/SSE/20240109/639898.dsb",
		make_cmp_v2_block_with_small_size_mismatch(BT_HIS_Trnsctn, pack_items(transactions)),
		"Sizechecking", trans, [&]() {
			return _reader.readTransSliceByRange("SSE.STK.639898", kStart, kEnd);
		});

	string unaligned_trans = pack_items(transactions);
	unaligned_trans.push_back('\x7f');
	expect_malformed_cmp_v2_rejected_and_retry<WTSTransStruct, WTSTransSlice>(
		_sink, BT_HIS_Trnsctn, "his/trans/SSE/20240109/639901.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, unaligned_trans),
		"incomplete record", trans, [&]() {
			return _reader.readTransSliceByRange("SSE.STK.639901", kStart, kEnd);
		});

	WTSTransStruct trans_zstd = make_trans(kTradingDate, kTradingDate, 93000000, 639902);
	expect_malformed_cmp_v2_rejected_and_retry<WTSTransStruct, WTSTransSlice>(
		_sink, BT_HIS_Trnsctn, "his/trans/SSE/20240109/639902.dsb",
		make_cmp_v2_block_with_compressed_payload(BT_HIS_Trnsctn, invalid_zstd),
		"Uncompressing", trans_zstd, [&]() {
			return _reader.readTransSliceByRange("SSE.STK.639902", kStart, kEnd);
		});

	expect_transform_failure_and_retry<WTSTransStruct, WTSTransSlice>(
		_sink, BT_HIS_Trnsctn, wtp::RdmDtReaderTransformFailureStage::BeforeResize,
		"his/trans/SSE/20240109/639910.dsb", trans, [&]() {
			return _reader.readTransSliceByRange("SSE.STK.639910", kStart, kEnd);
		});
	expect_transform_failure_and_retry<WTSTransStruct, WTSTransSlice>(
		_sink, BT_HIS_Trnsctn, wtp::RdmDtReaderTransformFailureStage::BeforeAppend,
		"his/trans/SSE/20240109/639911.dsb", trans, [&]() {
			return _reader.readTransSliceByRange("SSE.STK.639911", kStart, kEnd);
		});
}

// WtRdmDtReader.cpp: L831 / L883 / L1005 / L1057（四类缺失文件的早退）
TEST_F(R142RdmHftHisTest, R142RdmMissingRtAndHisHftFiles)
{
	// 当日 RT 文件缺失：isToday 分支的 getRT*Block 返回 NULL
	EXPECT_TRUE(_reader.readOrdDtlSliceByRange("SSE.STK.620802",
		20240110090000000ull, 20240110100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.630802",
		20240110090000000ull, 20240110100000000ull) == NULL);

	// 非当日历史文件缺失：his/orders|trans/<exchg>/<date>/<code>.dsb
	EXPECT_TRUE(_reader.readOrdDtlSliceByRange("SSE.STK.620803",
		20240109090000000ull, 20240109100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.630803",
		20240109090000000ull, 20240109100000000ull) == NULL);
}

// WtRdmDtReader.cpp: L711-716 长度校验 → L738-740 空块第二次访问
TEST_F(R142RdmHftHisTest, R142RdmEmptyQueueBlockSecondCallReturnsNull)
{
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/610804.dsb", string()));

	// 第一次：文件载荷短于 HisOrdQueBlockV2，清空缓存并返回；
	// 第二次：缓存项已存在但 _block 为空 -> L739-740。
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.610804",
		20240109090000000ull, 20240109100000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.610804",
		20240109090000000ull, 20240109100000000ull) == NULL);
}

// WtRdmDtReader.cpp: indexBarFromCacheByRange L1756-1766
TEST_F(R142RdmHftHisTest, R142RdmKlineHisEndBetweenBarsBacksOff)
{
	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240110, TimeUtils::timeToMinBar(20240110, 931), 1));
	bars.push_back(make_bar(20240110, TimeUtils::timeToMinBar(20240110, 933), 2));
	bars.push_back(make_bar(20240110, TimeUtils::timeToMinBar(20240110, 935), 3));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600805.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	// 截止 09:32 落在 09:31 与 09:33 之间：lower_bound 命中 09:33 后必须回退，
	// 结果只含 09:31。
	WTSKlineSlice* s = _reader.readKlineSliceByRange("SSE.STK.600805", KP_Minute1,
		202401100930ull, 202401100932ull);
	ASSERT_TRUE(s != NULL);
	ASSERT_EQ(s->size(), 1);
	EXPECT_EQ(s->at(0)->time, TimeUtils::timeToMinBar(20240110, 931));
	s->release();
}

} // namespace r142
