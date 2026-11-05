// L03-009: real writer rejects unverifiable session dates before effects.
#include "r23_writer_fixture.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {
class BlockingSink final : public r23::WriterSink {
public:
 explicit BlockingSink(IBaseDataMgr* bd) : WriterSink(bd) {}
 bool canSessionReceive(const char* session) override {
  std::unique_lock<std::mutex> lock(mu);
  if (!entered) { entered = true; cv.notify_all(); cv.wait(lock, [&] { return resume; }); }
  lock.unlock();
  return WriterSink::canSessionReceive(session);
 }
 bool waitEntered() { std::unique_lock<std::mutex> lock(mu); return cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }); }
 void unblock() { std::lock_guard<std::mutex> lock(mu); resume = true; cv.notify_all(); }
private:
 std::mutex mu; std::condition_variable cv; bool entered = false; bool resume = false;
};
class MutatingSink final : public r23::WriterSink {
public:
 explicit MutatingSink(IBaseDataMgr* bd) : WriterSink(bd) {}
 WTSTickData* tick = nullptr;
 bool canSessionReceive(const char*) override {
  tick->getTickStruct().action_date = 99991231;
  tick->getTickStruct().action_time = 230000000;
  return true;
 }
};
class L03WriterSessionDate : public r23::R23WriterTest {
protected:
 r23::TickSpec invalid() { r23::TickSpec s; s.exchg="SHFE"; s.code="fn2201"; s.action_date=99991231; s.trading_date=99991231; s.action_time=230000000; return s; }
};
TEST_F(L03WriterSessionDate, InvalidDateRejectedBeforeSynchronousEffectsAndCanRetry) {
 auto* w = make_writer(); auto* t = make_tick_data(invalid());
 const auto before = r23::read_file_bytes(cache_file());
 EXPECT_FALSE(w->writeTick(t, 0)); EXPECT_EQ(_sink->ticks(), 0u);
 EXPECT_EQ(r23::read_file_bytes(cache_file()), before);
 EXPECT_FALSE(r23::file_exists(rt_tick_file("fn2201")));
 r23::TickSpec good; good.exchg="SHFE"; good.code="fn2201"; good.action_date=20240108; good.trading_date=20240109; good.action_time=210000000;
 auto* accepted=make_tick_data(good); EXPECT_TRUE(w->writeTick(accepted,0)); EXPECT_EQ(_sink->ticks(),1u);
}
TEST_F(L03WriterSessionDate, InvalidDateRejectedBeforeAsyncAdmission) {
 auto* w=make_writer("\"async\":true"); auto* t=make_tick_data(invalid());
 const auto before=r23::read_file_bytes(cache_file());
 EXPECT_FALSE(w->writeTick(t,0)); close_writer(w);
 EXPECT_EQ(_sink->ticks(),0u); EXPECT_EQ(r23::read_file_bytes(cache_file()),before);
 EXPECT_FALSE(r23::file_exists(rt_tick_file("fn2201")));
}
TEST_F(L03WriterSessionDate, MissingAndMalformedDatesAndMissingSessionRejected) {
 auto* w=make_writer(); r23::TickSpec s;
 for (uint32_t d : {0u, 20240230u}) { s.action_date=d; auto* t=make_tick_data(s); EXPECT_FALSE(w->writeTick(t,0)); }
 auto* comm=_bd->getCommodity("SHFE","rb"); auto* session=comm->getSessionInfo(); session->retain(); comm->setSessionInfo(nullptr);
 s.action_date=20240108; auto* t=make_tick_data(s); EXPECT_FALSE(w->writeTick(t,0));
 comm->setSessionInfo(session); session->release(); EXPECT_EQ(_sink->ticks(),0u);
}
TEST_F(L03WriterSessionDate, AcceptedQueuedTickRecheckedBeforeEffects) {
 _sink.reset(new BlockingSink(_bd.get())); auto* blocking=static_cast<BlockingSink*>(_sink.get());
 auto* w=make_writer("\"async\":true"); r23::TickSpec first; auto* t1=make_tick_data(first);
 ASSERT_TRUE(w->writeTick(t1,0));
 if (!blocking->waitEntered()) { blocking->unblock(); close_writer(w); FAIL() << "worker did not enter callback"; return; }
 r23::TickSpec later; later.exchg="SHFE"; later.code="fn2201"; later.action_date=99991230; later.trading_date=99991231; later.action_time=210000000;
 auto* t2=make_tick_data(later);
 const bool admitted=w->writeTick(t2,0);
 EXPECT_TRUE(admitted);
 if (!admitted) { blocking->unblock(); close_writer(w); return; }
 // Synchronised with worker blocked in the previous sink callback.
 t2->getTickStruct().action_date=99991231; t2->getTickStruct().action_time=230000000;
 blocking->unblock(); close_writer(w);
 EXPECT_EQ(_sink->ticks(),1u); EXPECT_FALSE(r23::file_exists(rt_tick_file("fn2201")));
 EXPECT_TRUE(_sink->has_log("Cannot verify session date for queued tick", LL_ERROR));
}
TEST_F(L03WriterSessionDate, SynchronousSinkReentryCannotPublishInvalidDate) {
 _sink.reset(new MutatingSink(_bd.get())); auto* mutating=static_cast<MutatingSink*>(_sink.get());
 auto* w=make_writer(); r23::TickSpec s; s.exchg="SHFE"; s.code="fn2201";
 s.action_date=99991230; s.trading_date=99991231; s.action_time=210000000;
 auto* t=make_tick_data(s); mutating->tick=t;
 const auto before=r23::read_file_bytes(cache_file());
 EXPECT_FALSE(w->writeTick(t,0)); EXPECT_EQ(_sink->ticks(),0u);
 EXPECT_EQ(r23::read_file_bytes(cache_file()),before); EXPECT_FALSE(r23::file_exists(rt_tick_file("fn2201")));
 EXPECT_TRUE(_sink->has_log("Cannot verify session date after session check", LL_ERROR));
}
TEST_F(L03WriterSessionDate, MissingContractKeepsAcceptedNoOpBehavior) {
 auto* w=make_writer(); WTSTickStruct raw{};
 WTSTickData* tick=WTSTickData::create(raw);
 ASSERT_NE(tick,nullptr); EXPECT_TRUE(w->writeTick(tick,0));
 EXPECT_EQ(_sink->ticks(),0u); tick->release();
}
} // namespace
