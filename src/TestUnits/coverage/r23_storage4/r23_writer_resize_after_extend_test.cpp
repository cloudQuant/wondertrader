#include "r23_writer_fixture.h"
#include "../p10_storage_writer/p10_writer_test_hooks.h"

#include <fstream>
#include <limits>

namespace r23resize {

using namespace r23;

struct RemapFailureOnce
{
	bool pending = true;
	bool old_mapping_retained = false;
};

bool fail_next_remap(void* context, bool old_mapping_retained) noexcept
{
	auto* state = static_cast<RemapFailureOnce*>(context);
	if (state == nullptr || !state->pending)
		return false;
	state->old_mapping_retained = old_mapping_retained;
	state->pending = false;
	return true;
}

struct ResizeFailureSequence
{
	bool short_write_pending = false;
	bool short_write_injected = false;
	uint64_t short_write_bytes = 0;
	uint64_t file_size_before_short_write = 0;
	uint64_t requested_file_size = 0;
};

bool inject_short_write_once(void* context, const char* filename, uint64_t current_file_size,
	uint64_t requested_file_size) noexcept
{
	auto* state = static_cast<ResizeFailureSequence*>(context);
	if (state == nullptr || !state->short_write_pending || filename == nullptr
		|| requested_file_size <= current_file_size)
		return false;

	try
	{
		const uint64_t extension = requested_file_size - current_file_size;
		const uint64_t partial_size = extension / 2;
		if (partial_size == 0 || partial_size > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)()))
			return false;

		std::string partial(static_cast<std::size_t>(partial_size), '\0');
		std::ofstream output(filename, std::ios::binary | std::ios::out | std::ios::app);
		if (!output)
			return false;
		output.write(partial.data(), static_cast<std::streamsize>(partial.size()));
		output.close();
		if (!output)
			return false;

		state->short_write_pending = false;
		state->short_write_injected = true;
		state->short_write_bytes = partial_size;
		state->file_size_before_short_write = current_file_size;
		state->requested_file_size = requested_file_size;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

uint32_t session_action_time(uint32_t minute_index)
{
	const uint32_t absolute_minute = minute_index < 150
		? 9u * 60u + minute_index
		: 13u * 60u + (minute_index - 150u);
	const uint32_t hhmm = absolute_minute / 60u * 100u + absolute_minute % 60u;
	return hhmm * 100000u;
}

template<typename Record>
bool write_legacy_daily_block_with_uncommitted_tail(const string& path, uint16_t block_type,
	const Record& last_record)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	if (ec)
		return false;

	RTDayBlockHeader header {};
	std::memcpy(header._blk_flag, BLK_FLAG, sizeof(header._blk_flag));
	header._type = block_type;
	header._version = BLOCK_VERSION_RAW_V2;
	header._size = 2500;
	header._capacity = 2500;
	header._date = 20240108;

	std::vector<char> data(sizeof(Record) * 5000u, '\0');
	std::memcpy(data.data() + sizeof(Record) * 2499u, &last_record, sizeof(Record));
	std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::trunc);
	if (!output)
		return false;
	output.write(reinterpret_cast<const char*>(&header), sizeof(header));
	output.write(data.data(), static_cast<std::streamsize>(data.size()));
	output.close();
	return output.good();
}

struct HookCleanup
{
	WtDataWriter* writer;
	~HookCleanup() { p10_writer_test_hooks::uninstall(writer); }
};

class WriterResizeAfterExtendTest : public R23WriterTest
{
};

TEST_F(WriterResizeAfterExtendTest, RemapFailureAfterFileExtensionKeepsOldTicksAndCanRetry)
{
	R23Writer* writer = make_writer("\"groupsize\":100000");
	ASSERT_NE(writer, nullptr);

	TickSpec tick;
	tick.code = "rb2210";
	tick.exchg = "SHFE";
	tick.trading_date = 20240108;
	tick.action_date = 20240108;
	for (uint32_t i = 0; i < 2500; ++i)
	{
		tick.action_time = 90000000u + i * 1000u;
		tick.total_volume = 1000.0 + i;
		write_tick(writer, tick);
	}

	const string path = rt_tick_file("rb2210");
	const RTDayBlockHeader before = read_block_header<RTDayBlockHeader>(path);
	ASSERT_EQ(before._capacity, 2500u);
	ASSERT_EQ(before._size, 2500u);
	const uintmax_t original_size = file_size(path);
	ASSERT_GT(original_size, 0u);

	RemapFailureOnce failure;
	p10_writer_test_hooks::Hooks hooks;
	hooks.context = &failure;
	hooks.fail_next_rt_resize_map = fail_next_remap;
	ASSERT_TRUE(p10_writer_test_hooks::install(writer, hooks));
	HookCleanup cleanup{writer};

	// The call extends the same file, then deterministically rejects the new map.
	tick.action_time = 90000000u + 2500u * 1000u;
	tick.total_volume = 3500.0;
	write_tick(writer, tick);
	EXPECT_FALSE(failure.pending);
	EXPECT_TRUE(failure.old_mapping_retained);
	EXPECT_TRUE(_sink->has_log("Injected mapping failure after extending RT cache file", LL_ERROR));
	EXPECT_GT(file_size(path), original_size);

	const RTDayBlockHeader after_failure = read_block_header<RTDayBlockHeader>(path);
	EXPECT_EQ(after_failure._capacity, before._capacity);
	EXPECT_EQ(after_failure._size, before._size);
	WTSTickStruct preserved;
	const bool preserved_read = read_block_item<RTTickBlock, WTSTickStruct>(path, 2499, preserved);
	ASSERT_TRUE(preserved_read);
	EXPECT_EQ(preserved.total_volume, 3499.0);

	WTSTickData* latest = writer->getCurTick("rb2210", "SHFE");
	ASSERT_NE(latest, nullptr);
	EXPECT_EQ(latest->totalvolume(), 3500.0);
	latest->release();

	// The next unique tick remaps the expanded file and resumes at the old size.
	tick.action_time += 1000u;
	tick.total_volume += 1.0;
	write_tick(writer, tick);
	const RTDayBlockHeader after_retry = read_block_header<RTDayBlockHeader>(path);
	EXPECT_EQ(after_retry._capacity, 5000u);
	EXPECT_EQ(after_retry._size, 2501u);
	EXPECT_EQ(file_size(path),
		sizeof(RTDayBlockHeader) + sizeof(WTSTickStruct) * after_retry._capacity);
	WTSTickStruct retried;
	const bool retried_read = read_block_item<RTTickBlock, WTSTickStruct>(path, 2500, retried);
	ASSERT_TRUE(retried_read);
	EXPECT_EQ(retried.total_volume, 3501.0);
}

TEST_F(WriterResizeAfterExtendTest, ShortExtensionWithLegacyOversizedTailDoesNotPublishCapacityAndCanRetry)
{
	R23Writer* writer = make_writer("\"groupsize\":100000");
	ASSERT_NE(writer, nullptr);

	TickSpec tick;
	tick.code = "rb2210";
	tick.exchg = "SHFE";
	tick.trading_date = 20240108;
	tick.action_date = 20240108;
	for (uint32_t i = 0; i < 2500; ++i)
	{
		tick.action_time = 90000000u + i * 1000u;
		tick.total_volume = 1000.0 + i;
		write_tick(writer, tick);
	}

	const string path = rt_tick_file("rb2210");
	const RTDayBlockHeader original = read_block_header<RTDayBlockHeader>(path);
	ASSERT_EQ(original._capacity, 2500u);
	ASSERT_EQ(original._size, 2500u);
	const uintmax_t original_size = file_size(path);
	ASSERT_EQ(original_size,
		sizeof(RTDayBlockHeader) + sizeof(WTSTickStruct) * original._capacity);

	// Reproduce a file left by an older writer: the header still declares 2500
	// slots, while a stale, unused physical tail extends the file by 100 slots.
	const std::string historical_tail(sizeof(WTSTickStruct) * 100u, '\0');
	{
		std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::app);
		ASSERT_TRUE(output.good());
		output.write(historical_tail.data(), static_cast<std::streamsize>(historical_tail.size()));
		output.close();
		ASSERT_TRUE(output.good());
	}
	const uintmax_t legacy_oversized_size = file_size(path);
	ASSERT_EQ(legacy_oversized_size,
		original_size + sizeof(WTSTickStruct) * 100u);

	ResizeFailureSequence failure;
	p10_writer_test_hooks::Hooks hooks;
	hooks.context = &failure;
	hooks.simulate_next_rt_resize_short_write = inject_short_write_once;
	ASSERT_TRUE(p10_writer_test_hooks::install(writer, hooks));
	HookCleanup cleanup{writer};

	failure.short_write_pending = true;
	tick.action_time = 90000000u + 2500u * 1000u;
	tick.total_volume = 3500.0;
	write_tick(writer, tick);

	ASSERT_TRUE(failure.short_write_injected);
	EXPECT_GT(failure.short_write_bytes, 0u);
	EXPECT_LT(failure.short_write_bytes,
		failure.requested_file_size - failure.file_size_before_short_write);
	EXPECT_EQ(failure.file_size_before_short_write, legacy_oversized_size);
	EXPECT_EQ(failure.requested_file_size,
		sizeof(RTDayBlockHeader) + sizeof(WTSTickStruct) * 5000u);

	// A failed/short extension may leave the mmap physically unchanged in size
	// only after rollback; it must never publish capacity over an incomplete file.
	const RTDayBlockHeader after_short_write = read_block_header<RTDayBlockHeader>(path);
	EXPECT_EQ(after_short_write._capacity, original._capacity);
	EXPECT_EQ(after_short_write._size, original._size);
	EXPECT_EQ(file_size(path), legacy_oversized_size);
	WTSTickStruct preserved;
	ASSERT_TRUE((read_block_item<RTTickBlock, WTSTickStruct>(path, 2499, preserved)));
	EXPECT_EQ(preserved.total_volume, 3499.0);

	// The failed tick is not recorded in the old block. A subsequent tick retries
	// from the actual physical EOF, consumes the legacy tail, and finishes at
	// exactly the requested capacity rather than appending a stale full delta.
	tick.action_time += 1000u;
	tick.total_volume += 1.0;
	write_tick(writer, tick);
	const RTDayBlockHeader after_retry = read_block_header<RTDayBlockHeader>(path);
	EXPECT_EQ(after_retry._capacity, 5000u);
	EXPECT_EQ(after_retry._size, 2501u);
	EXPECT_EQ(file_size(path),
		sizeof(RTDayBlockHeader) + sizeof(WTSTickStruct) * after_retry._capacity);
	WTSTickStruct retried;
	ASSERT_TRUE((read_block_item<RTTickBlock, WTSTickStruct>(path, 2500, retried)));
	EXPECT_EQ(retried.total_volume, 3501.0);
}

TEST_F(WriterResizeAfterExtendTest, DailyRecordBlocksKeepOldPointerAndRetryAfterRemapFailure)
{
	R23Writer* writer = make_writer("\"groupsize\":100000");
	ASSERT_NE(writer, nullptr);

	for (uint32_t i = 0; i < 2500; ++i)
	{
		OrdQueSpec queue;
		queue.action_time += i * 1000u;
		queue.order_items = i + 1;
		WTSOrdQueData* event = make_ordque_data(queue);
		const bool accepted = writer->writeOrderQueue(event);
		event->release();
		ASSERT_TRUE(accepted);
	}
	for (uint32_t i = 0; i < 2500; ++i)
	{
		WTSOrdDtlData* event = make_orddtl_data(93000000u + i * 1000u, 3600.0 + i, i + 1);
		const bool accepted = writer->writeOrderDetail(event);
		event->release();
		ASSERT_TRUE(accepted);
	}
	for (uint32_t i = 0; i < 2500; ++i)
	{
		WTSTransData* event = make_trans_data(93000000u + i * 1000u, 3600.0 + i, i + 1);
		const bool accepted = writer->writeTransaction(event);
		event->release();
		ASSERT_TRUE(accepted);
	}

	const string queue_path = _base + "rt/queue/SHFE/rb2210.dmb";
	const string order_path = _base + "rt/orders/SHFE/rb2210.dmb";
	const string trans_path = _base + "rt/trans/SHFE/rb2210.dmb";
	ASSERT_EQ(read_block_header<RTDayBlockHeader>(queue_path)._size, 2500u);
	ASSERT_EQ(read_block_header<RTDayBlockHeader>(order_path)._size, 2500u);
	ASSERT_EQ(read_block_header<RTDayBlockHeader>(trans_path)._size, 2500u);

	RemapFailureOnce failure;
	failure.pending = false;
	p10_writer_test_hooks::Hooks hooks;
	hooks.context = &failure;
	hooks.fail_next_rt_resize_map = fail_next_remap;
	ASSERT_TRUE(p10_writer_test_hooks::install(writer, hooks));
	HookCleanup cleanup{writer};

	failure.pending = true;
	OrdQueSpec retry_queue;
	retry_queue.action_time = 93025000u;
	retry_queue.order_items = 2501;
	WTSOrdQueData* queue_event = make_ordque_data(retry_queue);
	const bool queue_failure_call_accepted = writer->writeOrderQueue(queue_event);
	queue_event->release();
	EXPECT_TRUE(queue_failure_call_accepted);
	EXPECT_FALSE(failure.pending);
	EXPECT_TRUE(failure.old_mapping_retained);
	EXPECT_EQ(_sink->queues(), 2500u);
	const RTDayBlockHeader queue_failed = read_block_header<RTDayBlockHeader>(queue_path);
	EXPECT_EQ(queue_failed._capacity, 2500u);
	EXPECT_EQ(queue_failed._size, 2500u);
	WTSOrdQueStruct old_queue;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSOrdQueStruct>(queue_path, 2499, old_queue)));
	EXPECT_EQ(old_queue.order_items, 2500u);

	retry_queue.action_time += 1000u;
	queue_event = make_ordque_data(retry_queue);
	const bool queue_retried = writer->writeOrderQueue(queue_event);
	queue_event->release();
	EXPECT_TRUE(queue_retried);
	const RTDayBlockHeader queue_after_retry = read_block_header<RTDayBlockHeader>(queue_path);
	EXPECT_EQ(queue_after_retry._capacity, 5000u);
	EXPECT_EQ(queue_after_retry._size, 2501u);
	EXPECT_EQ(file_size(queue_path),
		sizeof(RTDayBlockHeader) + sizeof(WTSOrdQueStruct) * queue_after_retry._capacity);
	EXPECT_EQ(_sink->queues(), 2501u);

	failure.pending = true;
	WTSOrdDtlData* order_event = make_orddtl_data(93025000u, 6100.0, 2501);
	const bool order_failure_call_accepted = writer->writeOrderDetail(order_event);
	order_event->release();
	EXPECT_TRUE(order_failure_call_accepted);
	EXPECT_FALSE(failure.pending);
	EXPECT_EQ(_sink->dtls(), 2500u);
	const RTDayBlockHeader order_failed = read_block_header<RTDayBlockHeader>(order_path);
	EXPECT_EQ(order_failed._capacity, 2500u);
	EXPECT_EQ(order_failed._size, 2500u);
	WTSOrdDtlStruct old_order;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSOrdDtlStruct>(order_path, 2499, old_order)));
	EXPECT_EQ(old_order.index, 2500u);

	order_event = make_orddtl_data(93026000u, 6101.0, 2502);
	const bool order_retried = writer->writeOrderDetail(order_event);
	order_event->release();
	EXPECT_TRUE(order_retried);
	const RTDayBlockHeader order_after_retry = read_block_header<RTDayBlockHeader>(order_path);
	EXPECT_EQ(order_after_retry._capacity, 5000u);
	EXPECT_EQ(order_after_retry._size, 2501u);
	EXPECT_EQ(file_size(order_path),
		sizeof(RTDayBlockHeader) + sizeof(WTSOrdDtlStruct) * order_after_retry._capacity);
	EXPECT_EQ(_sink->dtls(), 2501u);

	failure.pending = true;
	WTSTransData* trans_event = make_trans_data(93025000u, 6100.0, 2501);
	const bool trans_failure_call_accepted = writer->writeTransaction(trans_event);
	trans_event->release();
	EXPECT_TRUE(trans_failure_call_accepted);
	EXPECT_FALSE(failure.pending);
	EXPECT_EQ(_sink->trans(), 2500u);
	const RTDayBlockHeader trans_failed = read_block_header<RTDayBlockHeader>(trans_path);
	EXPECT_EQ(trans_failed._capacity, 2500u);
	EXPECT_EQ(trans_failed._size, 2500u);
	WTSTransStruct old_trans;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSTransStruct>(trans_path, 2499, old_trans)));
	EXPECT_EQ(old_trans.index, 2500u);

	trans_event = make_trans_data(93026000u, 6101.0, 2502);
	const bool trans_retried = writer->writeTransaction(trans_event);
	trans_event->release();
	EXPECT_TRUE(trans_retried);
	const RTDayBlockHeader trans_after_retry = read_block_header<RTDayBlockHeader>(trans_path);
	EXPECT_EQ(trans_after_retry._capacity, 5000u);
	EXPECT_EQ(trans_after_retry._size, 2501u);
	EXPECT_EQ(file_size(trans_path),
		sizeof(RTDayBlockHeader) + sizeof(WTSTransStruct) * trans_after_retry._capacity);
	EXPECT_EQ(_sink->trans(), 2501u);
}

TEST_F(WriterResizeAfterExtendTest, ReopenedDailyBlocksPreserveCommittedSizeAcrossExpandedTails)
{
	const string tick_path = rt_tick_file("rb2210");
	const string queue_path = _base + "rt/queue/SHFE/rb2210.dmb";
	const string order_path = _base + "rt/orders/SHFE/rb2210.dmb";
	const string trans_path = _base + "rt/trans/SHFE/rb2210.dmb";

	WTSTickStruct old_tick {};
	snprintf(old_tick.exchg, sizeof(old_tick.exchg), "%s", "SHFE");
	snprintf(old_tick.code, sizeof(old_tick.code), "%s", "rb2210");
	old_tick.trading_date = 20240108;
	old_tick.action_date = 20240108;
	old_tick.action_time = 92959000u;
	old_tick.price = 3599.0;
	old_tick.total_volume = 2499.0;
	WTSOrdQueStruct old_queue {};
	old_queue.order_items = 2500;
	old_queue.price = 4100.0;
	WTSOrdDtlStruct old_order {};
	old_order.index = 2500;
	old_order.orderid = 702500;
	old_order.price = 4200.0;
	WTSTransStruct old_trans {};
	old_trans.index = 2500;
	old_trans.price = 4300.0;

	// These files model a process restart after the physical expansion completed
	// but before its larger capacity was published into the header.
	ASSERT_TRUE(write_legacy_daily_block_with_uncommitted_tail(
		tick_path, BT_RT_Ticks, old_tick));
	ASSERT_TRUE(write_legacy_daily_block_with_uncommitted_tail(
		queue_path, BT_RT_OrdQueue, old_queue));
	ASSERT_TRUE(write_legacy_daily_block_with_uncommitted_tail(
		order_path, BT_RT_OrdDetail, old_order));
	ASSERT_TRUE(write_legacy_daily_block_with_uncommitted_tail(
		trans_path, BT_RT_Trnsctn, old_trans));

	R23Writer* writer = make_writer("\"groupsize\":100000");
	ASSERT_NE(writer, nullptr);

	TickSpec tick;
	tick.action_time = 93000000u;
	tick.total_volume = 2500.0;
	write_tick(writer, tick);
	const RTTickBlock tick_after = read_block_header<RTTickBlock>(tick_path);
	EXPECT_EQ(tick_after._capacity, 5000u);
	EXPECT_EQ(tick_after._size, 2501u);
	EXPECT_EQ(file_size(tick_path),
		sizeof(RTTickBlock) + sizeof(WTSTickStruct) * tick_after._capacity);
	WTSTickStruct tick_last;
	ASSERT_TRUE((read_block_item<RTTickBlock, WTSTickStruct>(tick_path, 2499, tick_last)));
	EXPECT_DOUBLE_EQ(tick_last.total_volume, 2499.0);
	WTSTickStruct tick_retried;
	ASSERT_TRUE((read_block_item<RTTickBlock, WTSTickStruct>(tick_path, 2500, tick_retried)));
	EXPECT_DOUBLE_EQ(tick_retried.total_volume, 2500.0);
	EXPECT_EQ(_sink->ticks(), 1u);

	OrdQueSpec queue;
	queue.action_time = 93000000u;
	queue.order_items = 3001;
	WTSOrdQueData* queue_event = make_ordque_data(queue);
	const bool queue_accepted = writer->writeOrderQueue(queue_event);
	queue_event->release();
	EXPECT_TRUE(queue_accepted);
	const RTDayBlockHeader queue_after = read_block_header<RTDayBlockHeader>(queue_path);
	EXPECT_EQ(queue_after._capacity, 5000u);
	EXPECT_EQ(queue_after._size, 2501u);
	EXPECT_EQ(file_size(queue_path),
		sizeof(RTDayBlockHeader) + sizeof(WTSOrdQueStruct) * queue_after._capacity);
	WTSOrdQueStruct queue_last;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSOrdQueStruct>(queue_path, 2499, queue_last)));
	EXPECT_EQ(queue_last.order_items, 2500u);
	WTSOrdQueStruct queue_retried;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSOrdQueStruct>(queue_path, 2500, queue_retried)));
	EXPECT_EQ(queue_retried.order_items, 3001u);
	EXPECT_EQ(_sink->queues(), 1u);

	WTSOrdDtlData* order_event = make_orddtl_data(93000000u, 6200.0, 3001u);
	const bool order_accepted = writer->writeOrderDetail(order_event);
	order_event->release();
	EXPECT_TRUE(order_accepted);
	const RTDayBlockHeader order_after = read_block_header<RTDayBlockHeader>(order_path);
	EXPECT_EQ(order_after._capacity, 5000u);
	EXPECT_EQ(order_after._size, 2501u);
	EXPECT_EQ(file_size(order_path),
		sizeof(RTDayBlockHeader) + sizeof(WTSOrdDtlStruct) * order_after._capacity);
	WTSOrdDtlStruct order_last;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSOrdDtlStruct>(order_path, 2499, order_last)));
	EXPECT_EQ(order_last.index, 2500u);
	WTSOrdDtlStruct order_retried;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSOrdDtlStruct>(order_path, 2500, order_retried)));
	EXPECT_EQ(order_retried.index, 3001u);
	EXPECT_EQ(_sink->dtls(), 1u);

	WTSTransData* trans_event = make_trans_data(93000000u, 6300.0, 3001u);
	const bool trans_accepted = writer->writeTransaction(trans_event);
	trans_event->release();
	EXPECT_TRUE(trans_accepted);
	const RTDayBlockHeader trans_after = read_block_header<RTDayBlockHeader>(trans_path);
	EXPECT_EQ(trans_after._capacity, 5000u);
	EXPECT_EQ(trans_after._size, 2501u);
	EXPECT_EQ(file_size(trans_path),
		sizeof(RTDayBlockHeader) + sizeof(WTSTransStruct) * trans_after._capacity);
	WTSTransStruct trans_last;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSTransStruct>(trans_path, 2499, trans_last)));
	EXPECT_EQ(trans_last.index, 2500u);
	WTSTransStruct trans_retried;
	ASSERT_TRUE((read_block_item<RTDayBlockHeader, WTSTransStruct>(trans_path, 2500, trans_retried)));
	EXPECT_EQ(trans_retried.index, 3001u);
	EXPECT_EQ(_sink->trans(), 1u);
}

TEST_F(WriterResizeAfterExtendTest, MinuteBlocksKeepOldDataAndRetryAfterRemapFailure)
{
	auto exercise = [&](const char* config, const string& relative_path,
		uint32_t expected_capacity, uint32_t retry_capacity, double starting_total_volume,
		uint32_t initial_trading_date, uint32_t initial_tick_count)
	{
		R23Writer* writer = make_writer(config);
		ASSERT_NE(writer, nullptr);
		TickSpec tick;
		tick.trading_date = initial_trading_date;
		tick.action_date = initial_trading_date;
		for (uint32_t i = 0; i < initial_tick_count; ++i)
		{
			tick.action_time = session_action_time(i);
			tick.price = 3600.0 + i;
			tick.total_volume = starting_total_volume + i;
			write_tick(writer, tick);
		}

		const string path = _base + relative_path;
		const RTKlineBlock before = read_block_header<RTKlineBlock>(path);
		ASSERT_EQ(before._capacity, expected_capacity);
		ASSERT_EQ(before._size, expected_capacity);
		WTSBarStruct previous;
		ASSERT_TRUE((read_block_item<RTKlineBlock, WTSBarStruct>(path, expected_capacity - 1, previous)));

		RemapFailureOnce failure;
		p10_writer_test_hooks::Hooks hooks;
		hooks.context = &failure;
		hooks.fail_next_rt_resize_map = fail_next_remap;
		ASSERT_TRUE(p10_writer_test_hooks::install(writer, hooks));
		HookCleanup cleanup{writer};

		tick.trading_date = initial_trading_date + 1;
		tick.action_date = initial_trading_date + 1;
		tick.action_time = 90000000u;
		tick.price += 1.0;
		tick.total_volume += 1.0;
		EXPECT_NO_THROW(write_tick(writer, tick));
		EXPECT_FALSE(failure.pending);
		EXPECT_TRUE(failure.old_mapping_retained);
		const RTKlineBlock after_failure = read_block_header<RTKlineBlock>(path);
		EXPECT_EQ(after_failure._capacity, before._capacity);
		EXPECT_EQ(after_failure._size, before._size);
		WTSBarStruct still_previous;
		ASSERT_TRUE((read_block_item<RTKlineBlock, WTSBarStruct>(path, expected_capacity - 1, still_previous)));
		EXPECT_EQ(still_previous.time, previous.time);
		EXPECT_DOUBLE_EQ(still_previous.close, previous.close);

		tick.action_time = 90100000u;
		tick.price += 1.0;
		tick.total_volume += 1.0;
		write_tick(writer, tick);
		const RTKlineBlock after_retry = read_block_header<RTKlineBlock>(path);
		EXPECT_EQ(after_retry._capacity, retry_capacity);
		EXPECT_EQ(after_retry._size, expected_capacity + 1u);
		EXPECT_EQ(file_size(path),
			sizeof(RTKlineBlock) + sizeof(WTSBarStruct) * after_retry._capacity);
		WTSBarStruct retried;
		ASSERT_TRUE((read_block_item<RTKlineBlock, WTSBarStruct>(path, expected_capacity, retried)));
		EXPECT_GT(retried.time, previous.time);
	};

	exercise("\"groupsize\":100000,\"disablemin5\":true",
		"rt/min1/SHFE/rb2210.dmb", 270u, 540u, 1000.0, 20240108u, 270u);
	exercise("\"groupsize\":100000,\"disablemin1\":true",
		"rt/min5/SHFE/rb2210.dmb", 54u, 108u, 2000.0, 20240110u, 266u);
}

} // namespace r23resize
