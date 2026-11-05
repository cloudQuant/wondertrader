/*
 * Iteration 03 (2026-11-04, package dt-writer): WtDataWriterAD writer lifecycle V1
 * (IDataWriter.h, decision card 9: 9a/9b/9d).
 *  - The exported V1 table validates handles and ordering (request_stop before
 *    wait_drained), reports WOULD_DEADLOCK on the worker, and DRAINED after the async
 *    worker has finished its accepted queue and was joined; admission is then closed.
 *  - release() called from a sink callback on the async worker must not self-join
 *    (old: std::system_error "Resource deadlock avoided" escaped into the callback).
 * -DS5_AD_BASELINE compiles only the cases that exist for the pre-change writer.
 */
#include "r4_ad_fixture.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>

#if !defined(S5_AD_BASELINE)
extern "C" IDataWriter* createWriter();
extern "C" void deleteWriter(IDataWriter*& writer);
extern "C" const WtDataWriterLifecycleApiV1* getDataWriterLifecycleApiV1(uint32_t requestedMajor);
#endif

namespace {

class I03WorkerCallbackSink : public R4WriterSink
{
public:
	using R4WriterSink::R4WriterSink;
	void broadcastTick(WTSTickData* tick) override
	{
		R4WriterSink::broadcastTick(tick);
		std::lock_guard<std::mutex> lock(_mutex);
		++_calls;
		if (_onTick)
			_onTick();
		_condition.notify_all();
	}
	bool waitCalls(uint32_t n)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, std::chrono::seconds(5), [&] { return _calls >= n; });
	}
	std::function<void()> _onTick;
	std::mutex _mutex;
	std::condition_variable _condition;
	uint32_t _calls = 0;
};

TEST_F(R4WriterADTest, I03AdReleaseFromWorkerCallbackDoesNotSelfJoin)
{
	I03WorkerCallbackSink sink(_bd);
	WtDataWriterAD writer;
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true, true);
	ASSERT_TRUE(writer.init(cfg, &sink));
	cfg->release();
	WTSCommodityInfo* comm = _bd->add_commodity("EXL", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXL", "rb01", comm);

	bool threw = false;
	std::string what;
	sink._onTick = [&]() {
		try
		{
			writer.release();	// on the async worker, inside the writer's own task
		}
		catch (const std::exception& ex)
		{
			threw = true;
			what = ex.what();
		}
	};
	WTSTickData* tick = tick_of(ct, make_tick("EXL", "rb01", 20230103, 20230103, 93000000, 100, 1, 1, 1, 1));
	ASSERT_TRUE(writer.writeTick(tick, 0));
	tick->release();
	ASSERT_TRUE(sink.waitCalls(1));
	EXPECT_FALSE(threw) << "release on the worker escaped: " << what;

	// Stop was requested from the callback: new writes are rejected; the owner drains.
	tick = tick_of(ct, make_tick("EXL", "rb01", 20230103, 20230103, 93001000, 101, 1, 1, 1, 1));
	EXPECT_FALSE(writer.writeTick(tick, 0));
	tick->release();
	writer.release();
	EXPECT_EQ(sink.bcast_tick(), 1u);
}

#if !defined(S5_AD_BASELINE)
TEST_F(R4WriterADTest, I03AdLifecycleV1TableDrainsAsyncWorker)
{
	const WtDataWriterLifecycleApiV1* api = getDataWriterLifecycleApiV1(WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR);
	ASSERT_NE(api, nullptr);
	EXPECT_EQ(getDataWriterLifecycleApiV1(2), nullptr);
	EXPECT_GE(api->struct_size, (uint32_t)sizeof(WtDataWriterLifecycleApiV1));
	EXPECT_EQ(api->abi_version >> 16, WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR);
	EXPECT_NE(api->capabilities & WT_DATA_WRITER_LIFECYCLE_V1_CAP_DRAIN, 0u);

	// A writer this module did not create is rejected.
	WtDataWriterAD foreign;
	EXPECT_EQ(api->request_stop(static_cast<void*>(static_cast<IDataWriter*>(&foreign))),
		WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT);

	I03WorkerCallbackSink sink(_bd);
	IDataWriter* writer = createWriter();
	ASSERT_NE(writer, nullptr);
	void* handle = static_cast<void*>(writer);
	WTSVariant* cfg = make_writer_cfg(_root.string(), 1000, true, true, true, true, true);
	ASSERT_TRUE(writer->init(cfg, &sink));
	cfg->release();
	WTSCommodityInfo* comm = _bd->add_commodity("EXV", "rb", ContractCategory::CC_Future);
	WTSContractInfo* ct = _bd->add_contract("EXV", "rb01", comm);

	std::atomic<int> canWaitOnWorker{ 99 };
	sink._onTick = [&]() { canWaitOnWorker = api->can_wait(handle); };
	for (uint32_t i = 0; i < 20; i++)
	{
		WTSTickData* tick = tick_of(ct, make_tick("EXV", "rb01", 20230103, 20230103, 93000000 + i * 1000, 100 + i, 1 + i, 1, 1, 1));
		EXPECT_TRUE(writer->writeTick(tick, 0));
		tick->release();
	}
	ASSERT_TRUE(sink.waitCalls(1));
	EXPECT_EQ(canWaitOnWorker.load(), WT_DATA_WRITER_LIFECYCLE_V1_WOULD_DEADLOCK);

	EXPECT_EQ(api->wait_drained(handle, 0), WT_DATA_WRITER_LIFECYCLE_V1_ERROR) << "stop must be requested first";
	EXPECT_EQ(api->request_stop(handle), WT_DATA_WRITER_LIFECYCLE_V1_OK);
	EXPECT_EQ(api->request_stop(handle), WT_DATA_WRITER_LIFECYCLE_V1_OK);	// idempotent
	EXPECT_EQ(api->can_wait(handle), WT_DATA_WRITER_LIFECYCLE_V1_OK);
	EXPECT_EQ(api->wait_drained(handle, 5000), WT_DATA_WRITER_LIFECYCLE_V1_DRAINED);
	EXPECT_EQ(sink.bcast_tick(), 20u) << "every accepted tick is processed before DRAINED";

	WTSTickData* late = tick_of(ct, make_tick("EXV", "rb01", 20230103, 20230103, 93100000, 200, 50, 1, 1, 1));
	EXPECT_FALSE(writer->writeTick(late, 0));
	late->release();
	WTSVariant* again = make_writer_cfg(_root.string(), 1000, true, true, true, true, true);
	EXPECT_FALSE(writer->init(again, &sink)) << "a stopped writer is not re-initialized";
	again->release();

	writer->release();
	deleteWriter(writer);
	EXPECT_EQ(writer, nullptr);
	EXPECT_EQ(api->request_stop(handle), WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT);
}
#endif

} // namespace
