/*
 * BUG-P10-006（迭代03 最终缺口，slice2；决策 K20=B）——普通写端 WtDataWriter 的外部 dumper
 * 注册与遍历同步。收盘作业（transHisData）对每类数据各取一次注册表快照、在注册锁外调用：
 * d1/m1/m5 K 线、逐笔 tick、逐笔成交、委托明细、委托队列。
 * 确定性交错：dumper A 在收盘作业里的第一次回调中阻塞，另一线程此时登记/注销 dumper：
 *   - 登记与注销在有界时间内返回（回调期间不持注册锁，不死锁）；
 *   - A 阻塞的那一处用旧快照：新登记的 B 收不到该类，其后每一类恰好收到一次；
 *   - 重复 ID 被拒并保留原实例；注销后不再被调用，进行中的调用可完成；
 *   - 回调内重入登记不死锁，被登记者自下一类起收到。
 * TSan 下重复运行由主代理执行（见证据 README）。新接口用例，改前无对应 API，红控不适用。
 */
#include "r23_writer_fixture.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace i03s2p10006 {

using namespace r23;
using std::string;
using std::vector;

class RecordingDumper : public IHisDataDumper
{
public:
	// 第一次回调时阻塞，直到 release()；onFirst 在阻塞前于回调线程上执行
	void block_first_call() { _block = true; }
	std::function<void()> onFirst;

	bool dumpHisBars(const char*, const char* period, WTSBarStruct*, uint32_t) override { return record(string("bars:") + period); }
	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override { return record("ticks"); }
	bool dumpHisOrdQue(const char*, uint32_t, WTSOrdQueStruct*, uint32_t) override { return record("ordque"); }
	bool dumpHisOrdDtl(const char*, uint32_t, WTSOrdDtlStruct*, uint32_t) override { return record("orddtl"); }
	bool dumpHisTrans(const char*, uint32_t, WTSTransStruct*, uint32_t) override { return record("trans"); }

	bool wait_blocked(int ms)
	{
		std::unique_lock<std::mutex> lock(_mtx);
		return _cv.wait_for(lock, std::chrono::milliseconds(ms), [this]() { return _blocked; });
	}
	void release()
	{
		std::lock_guard<std::mutex> lock(_mtx);
		_released = true;
		_cv.notify_all();
	}
	vector<string> calls()
	{
		std::lock_guard<std::mutex> lock(_mtx);
		return _calls;
	}

private:
	bool record(const string& kind)
	{
		bool first = false;
		{
			std::lock_guard<std::mutex> lock(_mtx);
			_calls.push_back(kind);
			first = _calls.size() == 1;
		}
		if (first && onFirst)
			onFirst();
		if (first && _block)
		{
			std::unique_lock<std::mutex> lock(_mtx);
			_blocked = true;
			_cv.notify_all();
			_cv.wait_for(lock, std::chrono::seconds(20), [this]() { return _released; });
		}
		return true;
	}

	std::mutex _mtx;
	std::condition_variable _cv;
	vector<string> _calls;
	bool _block = false;
	bool _blocked = false;
	bool _released = false;
};

const vector<string>& all_kinds()
{
	static const vector<string> kinds = { "bars:d1", "bars:m1", "bars:m5", "ticks", "trans", "orddtl", "ordque" };
	return kinds;
}

uint32_t count_of(const vector<string>& calls, const string& kind)
{
	uint32_t n = 0;
	for (const string& c : calls)
		n += c == kind ? 1 : 0;
	return n;
}

class I03S2P10006DumperConcurrency : public R23WriterTest
{
protected:
	R23Writer* writer_with_all_streams()
	{
		R23Writer* w = make_writer();
		EXPECT_TRUE(w != NULL);
		TickSpec t1;
		write_tick(w, t1, 0);
		TickSpec t2;
		t2.action_time = 93100000;
		t2.total_volume = 1100.0;
		write_tick(w, t2, 0);
		WTSTransData* tr = make_trans_data(93000000, 3650.0, 7);
		EXPECT_TRUE(w->writeTransaction(tr));
		tr->release();
		WTSOrdDtlData* od = make_orddtl_data(93000000, 3640.0, 3);
		EXPECT_TRUE(w->writeOrderDetail(od));
		od->release();
		OrdQueSpec qs;
		WTSOrdQueData* oq = make_ordque_data(qs);
		EXPECT_TRUE(w->writeOrderQueue(oq));
		oq->release();
		return w;
	}

	// 在有界时间内执行登记类调用；超时说明回调期间仍持注册锁（死锁）
	static WtDataWriterLifecycleV1Status bounded(std::function<WtDataWriterLifecycleV1Status()> fn, bool& timedOut)
	{
		auto fut = std::async(std::launch::async, fn);
		timedOut = fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready;
		return timedOut ? WT_DATA_WRITER_LIFECYCLE_V1_ERROR : fut.get();
	}
};

} // namespace i03s2p10006

using namespace i03s2p10006;

TEST_F(I03S2P10006DumperConcurrency, RegistrationWhileACloseDumpIsRunningUsesExactSnapshots)
{
	R23Writer* w = writer_with_all_streams();
	ASSERT_TRUE(w != NULL);
	RecordingDumper a, b, dup;
	a.block_first_call();
	ASSERT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, w->addDumperV1("A", &a));

	abandon_writer(w);
	w->transHisData("S2_P10006_A");
	ASSERT_TRUE(a.wait_blocked(10000)) << "close job never called the dumper";

	bool timedOut = false;
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, bounded([&]() { return w->addDumperV1("B", &b); }, timedOut));
	EXPECT_FALSE(timedOut) << "registration blocked while a dumper callback was running";
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_DUPLICATE, bounded([&]() { return w->addDumperV1("A", &dup); }, timedOut));
	EXPECT_FALSE(timedOut);
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT, bounded([&]() { return w->removeDumperV1("missing"); }, timedOut));
	EXPECT_FALSE(timedOut);

	a.release();
	ASSERT_TRUE(wait_marker("S2_P10006_A"));

	const vector<string> ca = a.calls();
	const vector<string> cb = b.calls();
	ASSERT_FALSE(ca.empty());
	for (const string& kind : all_kinds())
		EXPECT_EQ(1u, count_of(ca, kind)) << "A must receive " << kind << " exactly once";
	EXPECT_EQ(ca.size(), all_kinds().size());
	// B 登记于 A 的首个回调期间：该处快照不含 B，其后的每一类 B 恰好一次
	EXPECT_EQ(0u, count_of(cb, ca.front())) << "the snapshot taken before registration must not include B";
	for (size_t i = 1; i < ca.size(); i++)
		EXPECT_EQ(1u, count_of(cb, ca[i])) << "B must receive " << ca[i] << " exactly once";
	EXPECT_EQ(cb.size(), ca.size() - 1);
	EXPECT_TRUE(dup.calls().empty()) << "a rejected duplicate must never be called";
}

TEST_F(I03S2P10006DumperConcurrency, RemovalDuringACallbackLetsItFinishAndStopsLaterCalls)
{
	R23Writer* w = writer_with_all_streams();
	ASSERT_TRUE(w != NULL);
	RecordingDumper a, keep;
	a.block_first_call();
	ASSERT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, w->addDumperV1("A", &a));
	ASSERT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, w->addDumperV1("keep", &keep));

	abandon_writer(w);
	w->transHisData("S2_P10006_R");
	ASSERT_TRUE(a.wait_blocked(10000));
	bool timedOut = false;
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, bounded([&]() { return w->removeDumperV1("A"); }, timedOut));
	EXPECT_FALSE(timedOut) << "removal blocked while the removed dumper's callback was running";
	a.release();
	ASSERT_TRUE(wait_marker("S2_P10006_R"));

	EXPECT_EQ(1u, a.calls().size()) << "the in-flight call finishes; no later call reaches a removed dumper";
	const vector<string> ck = keep.calls();
	for (const string& kind : all_kinds())
		EXPECT_EQ(1u, count_of(ck, kind)) << "the remaining dumper must receive " << kind << " exactly once";
}

TEST_F(I03S2P10006DumperConcurrency, RegistrationFromInsideACloseDumpCallbackDoesNotDeadlock)
{
	R23Writer* w = writer_with_all_streams();
	ASSERT_TRUE(w != NULL);
	RecordingDumper a, late;
	WtDataWriterLifecycleV1Status reentrant = WT_DATA_WRITER_LIFECYCLE_V1_ERROR;
	a.onFirst = [&]() { reentrant = w->addDumperV1("late", &late); };
	ASSERT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, w->addDumperV1("A", &a));

	abandon_writer(w);
	w->transHisData("S2_P10006_C");
	ASSERT_TRUE(wait_marker("S2_P10006_C")) << "reentrant registration from the close job deadlocked";
	EXPECT_EQ(WT_DATA_WRITER_LIFECYCLE_V1_OK, reentrant);

	const vector<string> ca = a.calls();
	ASSERT_EQ(all_kinds().size(), ca.size());
	const vector<string> cl = late.calls();
	EXPECT_EQ(0u, count_of(cl, ca.front()));
	EXPECT_EQ(ca.size() - 1, cl.size()) << "the reentrantly registered dumper receives every later kind once";
}
