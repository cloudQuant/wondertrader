/*!
 * \file s4_ctploader_release_order_test.cpp
 * I03-NEW-123 (found by slice2, fixed in slice4 final gaps): CTPLoader run() released
 * pUserApi and set it to NULL before waiting for the SPI's query worker, while a query
 * task the worker had already committed still called pUserApi. run()'s teardown is now
 * ctp_loader_release(): stop and wait for the workers first, then RegisterSpi(NULL) /
 * Release() / pUserApi = NULL / delete; a worker still busy after the bound leaves the
 * API and the SPI untouched (retained until process exit, card 9a).
 *
 * The API is a local stub (no vendor API, no network). Its ReqQryInstrument holds the
 * worker inside the call (bounded) and records a Release() that arrives meanwhile.
 */
#include "t26_ctploader_common.hpp"
#include "t26_ctp_api_stub.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace
{
	class S4HoldingApi : public T26CtpApiStub
	{
	public:
		explicit S4HoldingApi(std::chrono::milliseconds hold) : _hold(hold) {}

		int ReqQryInstrument(CThostFtdcQryInstrumentField* pField, int nRequestID) override
		{
			std::unique_lock<std::mutex> lock(_mtx);
			_in_call = true;
			_entered = true;
			_cv.notify_all();
			// A Release() during the call ends the hold early (the call would now run
			// on a released API); otherwise the call returns after the hold.
			_cv.wait_for(lock, _hold, [this]() { return _released; });
			_in_call = false;
			lock.unlock();
			return T26CtpApiStub::ReqQryInstrument(pField, nRequestID);
		}

		void RegisterSpi(CThostFtdcTraderSpi* spi) override
		{
			std::lock_guard<std::mutex> lock(_mtx);
			if (spi == nullptr && _in_call)
				_detached_during_call = true;
		}

		void Release() override
		{
			std::lock_guard<std::mutex> lock(_mtx);
			++_releases;
			if (_in_call)
				_released_during_call = true;
			_released = true;
			_cv.notify_all();
		}

		bool waitEntered(std::chrono::milliseconds timeout)
		{
			std::unique_lock<std::mutex> lock(_mtx);
			return _cv.wait_for(lock, timeout, [this]() { return _entered; });
		}

		int releases() { std::lock_guard<std::mutex> lock(_mtx); return _releases; }
		bool releasedDuringCall() { std::lock_guard<std::mutex> lock(_mtx); return _released_during_call; }
		bool detachedDuringCall() { std::lock_guard<std::mutex> lock(_mtx); return _detached_during_call; }

	private:
		std::chrono::milliseconds _hold;
		std::mutex _mtx;
		std::condition_variable _cv;
		bool _in_call = false;
		bool _entered = false;
		bool _released = false;
		bool _released_during_call = false;
		bool _detached_during_call = false;
		int _releases = 0;
	};

	CThostFtdcRspUserLoginField s4LoginRsp()
	{
		CThostFtdcRspUserLoginField rsp;
		std::memset(&rsp, 0, sizeof(rsp));
		rsp.FrontID = 1;
		rsp.SessionID = 2;
		std::snprintf(rsp.MaxOrderRef, sizeof(rsp.MaxOrderRef), "%s", "1");
		return rsp;
	}

	// Logs in through the real SPI path; the queued instrument query is then executed
	// by the SPI's own worker, which is held inside pUserApi->ReqQryInstrument.
	void s4StartHeldQuery(CTraderSpi* spi, S4HoldingApi& api)
	{
		CThostFtdcRspUserLoginField login = s4LoginRsp();
		CThostFtdcRspInfoField ok = t26::ok_rsp();
		spi->OnRspUserLogin(&login, &ok, 1, true);
		ASSERT_TRUE(api.waitEntered(std::chrono::seconds(5))) << "fixture: the worker reached the API call";
	}
}

TEST(S4CTPLoaderReleaseOrder, workers_are_stopped_before_the_api_is_released)
{
	t26::setup_defaults("s4_release_order");
	t26::FdRedirectGuard workerNoise(SAVEPATH + "worker_noise.log");
	S4HoldingApi* api = new S4HoldingApi(std::chrono::milliseconds(300));	// outlives any worker
	pUserApi = api;
	CTraderSpi* spi = new CTraderSpi();
	ASSERT_NO_FATAL_FAILURE(s4StartHeldQuery(spi, *api));

	EXPECT_TRUE(ctp_loader_release(spi, std::chrono::seconds(5)));
	EXPECT_FALSE(api->releasedDuringCall()) << "Release() reached the API while the worker was still calling it";
	EXPECT_FALSE(api->detachedDuringCall()) << "RegisterSpi(NULL) reached the API while the worker was still calling it";
	EXPECT_EQ(1, api->releases());
	EXPECT_EQ(1u, api->n_qry_inst_) << "the committed query completed on a live API";
	EXPECT_EQ(nullptr, pUserApi);
	pUserApi = NULL;
}

TEST(S4CTPLoaderReleaseOrder, a_worker_still_busy_after_the_bound_keeps_the_api_alive)
{
	t26::setup_defaults("s4_release_busy");
	t26::FdRedirectGuard workerNoise(SAVEPATH + "worker_noise.log");
	S4HoldingApi* api = new S4HoldingApi(std::chrono::milliseconds(1500));	// outlives any worker
	pUserApi = api;
	CTraderSpi* spi = new CTraderSpi();
	ASSERT_NO_FATAL_FAILURE(s4StartHeldQuery(spi, *api));

	const bool retained = !ctp_loader_release(spi, std::chrono::milliseconds(100));
	EXPECT_TRUE(retained) << "a worker still inside the API call must be reported";
	EXPECT_EQ(0, api->releases()) << "an API still in use by a worker is never released";
	EXPECT_FALSE(api->releasedDuringCall());
	EXPECT_FALSE(api->detachedDuringCall());
	EXPECT_EQ(static_cast<CThostFtdcTraderApi*>(api), pUserApi) << "the worker's API pointer stays valid";

	// Test cleanup only: the retained SPI finishes once the held call returns.
	if (retained)
	{
		ASSERT_TRUE(spi->WaitWorkersStopped(std::chrono::seconds(5)));
		EXPECT_EQ(1u, api->n_qry_inst_);
		delete spi;
	}
	pUserApi = NULL;
}
