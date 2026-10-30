/*!
 * /file WtUftRunner.cpp
 * /project	WonderTrader
 *
 * /author Wesley
 * /date 2020/03/30
 * 
 * /brief 
 */
#include "UftLatencyTool.h"
#include "../WtUftCore/UftStraContext.h"

#include <exception>

#include "../Includes/WTSVariant.hpp"
#include "../Includes/IParserApi.h"
#include "../Includes/ITraderApi.h"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"

#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CpuHelper.hpp"


USING_NS_WTP;

void test_uft()
{
	uft::UftLatencyTool runner;
	runner.init();

	runner.run();
}

namespace uft
{
	class TestParser : public IParserApi
	{
	public:
		virtual bool init(WTSVariant* /*cfg*/) override
		{
			return true;
		}

		void	run(uint32_t times)
		{
			if (times == 0)
			{
				WTSLogger::warn("0 ticks simulated; average latency unavailable (no samples)");
				return;
			}

			srand(time(NULL));
			TimeUtils::Ticker ticker;
			for (uint32_t i = 0; i < times; i++)
			{
				uint32_t actDate = 20220303;// strtoul("20220303", NULL, 10);
				uint32_t actTime = 100523 * 1000 + 500; //strToTime("10:05:23") * 1000 + 500;

				WTSContractInfo* contract = _bd_mgr->getContract("rb2205", "SHFE");
				if (contract == NULL)
				{
					WTSLogger::warn("Skipping latency test tick: contract {}.{} not found", "SHFE", "rb2205");
					return;
				}

				double x = rand();

				WTSCommodityInfo* pCommInfo = contract->getCommInfo();
				if (pCommInfo == NULL)
				{
					WTSLogger::warn("Skipping latency test tick: contract {}.{} has no commodity info", "SHFE", "rb2205");
					return;
				}

				WTSTickData* tick = WTSTickData::create("rb2205");
				tick->setContractInfo(contract);

				WTSTickStruct& quote = tick->getTickStruct();
				wt_strcpy(quote.exchg, pCommInfo->getExchg());

				quote.action_date = actDate;
				quote.action_time = actTime;

				quote.price = x;
				quote.open = x;
				quote.high = x;
				quote.low = x;
				quote.total_volume = 0;
				quote.trading_date = 20220303;
				quote.settle_price = x;

				quote.open_interest = 0;

				quote.upper_limit = x;
				quote.lower_limit = x;

				quote.pre_close = x;
				quote.pre_settle = x;
				quote.pre_interest = 0;

				//委卖价格
				quote.ask_prices[0] = x;
				quote.ask_prices[1] = x;
				quote.ask_prices[2] = x;
				quote.ask_prices[3] = x;
				quote.ask_prices[4] = x;

				//委买价格
				quote.bid_prices[0] = x;
				quote.bid_prices[1] = x;
				quote.bid_prices[2] = x;
				quote.bid_prices[3] = x;
				quote.bid_prices[4] = x;

				//委卖量
				quote.ask_qty[0] = 0;
				quote.ask_qty[1] = 0;
				quote.ask_qty[2] = 0;
				quote.ask_qty[3] = 0;
				quote.ask_qty[4] = 0;

				//委买量
				quote.bid_qty[0] = 0;
				quote.bid_qty[1] = 0;
				quote.bid_qty[2] = 0;
				quote.bid_qty[3] = 0;
				quote.bid_qty[4] = 0;

				_parser_spi->handleQuote(tick, 0);
				tick->release();
			}
			auto total = ticker.nano_seconds();
			double t2t = total * 1.0 / times;
			WTSLogger::warn("{} ticks simulated in {:.0f} ns, UftEngine Innner Latency: {:.3f} ns", times, total*1.0, t2t);
		}

	public:
		virtual void registerSpi(IParserSpi* listener) override
		{
			_parser_spi = listener;
			_bd_mgr = listener->getBaseDataMgr();
		}

	private:
		IParserSpi*		_parser_spi;
		IBaseDataMgr*	_bd_mgr;
	};

	class TestTrader : public ITraderApi
	{
	public:
		virtual bool init(WTSVariant* /*cfg*/) override { return true; }

		virtual void registerSpi(ITraderSpi* listener) override
		{
			_trader_spi = listener;
		}

		virtual void connect() override
		{
			if (_trader_spi != NULL)
				_trader_spi->onLoginResult(true, "ok", 0);
		}

		virtual int queryPositions() override
		{
			WTSArray* empty = WTSArray::create();
			_trader_spi->onRspPosition(empty);
			empty->release();
			return 0;
		}

		virtual int queryOrders() override
		{
			WTSArray* empty = WTSArray::create();
			_trader_spi->onRspOrders(empty);
			empty->release();
			return 0;
		}

		virtual int queryTrades() override
		{
			WTSArray* empty = WTSArray::create();
			_trader_spi->onRspTrades(empty);
			empty->release();
			return 0;
		}

		virtual int queryAccount() override
		{
			WTSArray* empty = WTSArray::create();
			_trader_spi->onRspAccount(empty);
			empty->release();
			return 0;
		}

		virtual bool makeEntrustID(char* buffer, int /*length*/) override
		{
			wt_strcpy(buffer, "123456");
			return true;
		}

		virtual int orderInsert(WTSEntrust* /*eutrust*/) override
		{
			//WTSLogger::debug("{}", __FUNCTION__);
			return 0;
		}

	private:
		ITraderSpi*	_trader_spi;
	};

	class TestStrategy : public UftStrategy
	{
	public:
		TestStrategy(const char* id) : UftStrategy(id) {}

		/*
		*	执行单元名称
		*/
		virtual const char* getName() override { return "TestStrategy"; }

		/*
		*	所属执行器工厂名称
		*/
		virtual const char* getFactName() override { return "TestStrategyFact"; }


		virtual void on_init(IUftStraCtx* ctx) override
		{
			ctx->stra_sub_ticks("SHFE.rb2205");
		}

		virtual void on_tick(IUftStraCtx* ctx, const char* /*code*/, WTSTickData* /*newTick*/) override
		{
			//WTSLogger::debug("{}", __FUNCTION__);
			ctx->stra_enter_long("SHFE.rb2205", 2300, 1, 0);
			//ctx->stra_enter_short("SHFE.rb2205", 2300, 1, 0);
		}
	};


	UftLatencyTool::UftLatencyTool()
	{
		_init_attempted = false;
	}


	UftLatencyTool::~UftLatencyTool()
	{
		if (!shutdown())
			std::terminate();
	}

	bool UftLatencyTool::shutdown()
	{
		if (_shutdown_complete.load(std::memory_order_acquire))
			return true;

		_shutdown_requested.store(true, std::memory_order_release);
		_traders.requestStopSinkNotifications();

		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			if (_operation_active && _operation_thread == std::this_thread::get_id())
				return false;
		}

		// The manager preflights callback/API reentrancy before blocking. A callback
		// that requested shutdown leaves cleanup to a control thread or operation exit.
		if (!_traders.waitSinkNotificationsStopped())
			return false;

		{
			std::unique_lock<std::mutex> lock(_lifecycle_mutex);
			_lifecycle_cv.wait(lock, [this]() { return !_operation_active; });
			if (_shutdown_complete.load(std::memory_order_acquire))
				return true;
		}

		// Detect ticker-callback reentrancy before claiming teardown ownership. A
		// ticker callback must return false here instead of waiting for a controller
		// that may itself be joining this callback's worker.
		_engine.request_stop();
		if (!_engine.wait_stopped())
			return false;

		{
			std::unique_lock<std::mutex> lock(_lifecycle_mutex);
			if (_shutdown_complete.load(std::memory_order_acquire))
				return true;
			while (_shutdown_in_progress)
			{
				if (_shutdown_thread == std::this_thread::get_id())
					return false;
				_lifecycle_cv.wait(lock, [this]() {
					return !_shutdown_in_progress || _shutdown_complete.load(std::memory_order_acquire);
				});
				if (_shutdown_complete.load(std::memory_order_acquire))
					return true;
			}
			_shutdown_in_progress = true;
			_shutdown_thread = std::this_thread::get_id();
		}

		bool stopped = false;
		try
		{
			if (!_inputs_stopped.load(std::memory_order_acquire))
			{
				// Keep the borrowed parser alive until run() has left its operation lease.
				_parsers.release();
				_engine.stop();
				{
					std::lock_guard<std::mutex> lock(_lifecycle_mutex);
					_test_parser = nullptr;
				}
				_inputs_stopped.store(true, std::memory_order_release);
			}

			if (_traders.waitSinkNotificationsStopped())
			{
				_traders.release();
				_test_trader_api.reset();
				stopped = true;
			}
		}
		catch (...)
		{
			WTSLogger::error("UFT latency tool shutdown raised an exception; resources remain owned");
		}

		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			if (stopped)
				_shutdown_complete.store(true, std::memory_order_release);
			_shutdown_in_progress = false;
			_shutdown_thread = std::thread::id();
		}
		_lifecycle_cv.notify_all();
		return stopped;
	}

	bool UftLatencyTool::init()
	{
		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			// Reject reinitialization or any start after shutdown without mutating state.
			if (_init_attempted || _shutdown_requested.load(std::memory_order_acquire)
				|| _shutdown_complete.load(std::memory_order_acquire))
			{
				WTSLogger::error("UFT latency tool initialization can only be attempted once before shutdown");
				return false;
			}
			_init_attempted = true;
			_operation_active = true;
			_operation_thread = std::this_thread::get_id();
		}

		bool initialized = false;
		try
		{
			initialized = initImpl();
		}
		catch (...)
		{
			WTSLogger::error("UFT latency tool initialization raised an exception");
			initialized = false;
		}

		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			_operation_active = false;
			_operation_thread = std::thread::id();
			if (!initialized)
				_shutdown_requested.store(true, std::memory_order_release);
		}
		_lifecycle_cv.notify_all();

		if (!initialized || _shutdown_requested.load(std::memory_order_acquire))
			shutdown();
		return initialized && !_shutdown_requested.load(std::memory_order_acquire);
	}

	bool UftLatencyTool::initImpl()
	{
		WTSLogger::init("logcfg.yaml");

		std::string cfgFile = "config.yaml";

		WTSVariant* _config = WTSCfgLoader::load_from_file(cfgFile);
		if (_config == NULL)
		{
			WTSLogger::error("Loading config file {} failed", cfgFile);
			return false;
		}

		//基础数据文件
		WTSVariant* cfgBF = _config->get("basefiles");
		if (cfgBF->get("session"))
			_bd_mgr.loadSessions(cfgBF->getCString("session"));

		WTSVariant* cfgItem = cfgBF->get("commodity");
		if (cfgItem)
		{
			if (cfgItem->type() == WTSVariant::VT_String)
			{
				_bd_mgr.loadCommodities(cfgItem->asCString());
			}
			else if (cfgItem->type() == WTSVariant::VT_Array)
			{
				for (uint32_t i = 0; i < cfgItem->size(); i++)
				{
					_bd_mgr.loadCommodities(cfgItem->get(i)->asCString());
				}
			}
		}

		cfgItem = cfgBF->get("contract");
		if (cfgItem)
		{
			if (cfgItem->type() == WTSVariant::VT_String)
			{
				_bd_mgr.loadContracts(cfgItem->asCString());
			}
			else if (cfgItem->type() == WTSVariant::VT_Array)
			{
				for (uint32_t i = 0; i < cfgItem->size(); i++)
				{
					_bd_mgr.loadContracts(cfgItem->get(i)->asCString());
				}
			}
		}

		_times = _config->getUInt32("times");
		WTSLogger::warn("{} ticks will be simulated", _times);

		_core = _config->getUInt32("core");
		WTSLogger::warn("Testing thread will be bind to core {}", _core);

		if (!initEngine(_config->get("env")))
		{
			_config->release();
			return false;
		}
		if (!initModules() || !initStrategies())
		{
			_config->release();
			return false;
		}

		_config->release();
		return true;
	}

	bool UftLatencyTool::initStrategies()
	{
		UftStraContext* ctx = new UftStraContext(&_engine, "stra");
		UftContextPtr context(ctx);
		ctx->set_strategy(new TestStrategy("stra"));

		TraderAdapterPtr trader = _traders.getAdapter("trader");
		if (!trader)
		{
			WTSLogger::error("Latency strategy trader adapter was not registered");
			return false;
		}
		ctx->setTrader(trader.get());
		_engine.addContext(context);
		trader->addSink(ctx);

		return true;
	}

	bool UftLatencyTool::initEngine(WTSVariant* cfg)
	{
		WTSLogger::warn("Trading enviroment initialzied with engine: UFT");
		_engine.init(cfg, &_bd_mgr, NULL, NULL);
		_engine.set_adapter_mgr(&_traders);

		return true;
	}


	bool UftLatencyTool::initModules()
	{
		{
			TestParser* parser = new TestParser();
			ParserAdapterPtr adapter(new ParserAdapter);
			// initExt owns and destroys the external API on post-assignment failure.
			// Publish the borrowed pointer only after adapter registration succeeds.
			if (!adapter->initExt("parser", parser, &_engine, &_bd_mgr))
			{
				WTSLogger::error("Initializing latency test parser adapter failed");
				return false;
			}

			if (!_parsers.addAdapter("parser", adapter))
			{
				adapter->release();
				WTSLogger::error("Registering latency test parser adapter failed");
				return false;
			}
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			_test_parser = parser;
		}

		{
			std::unique_ptr<TestTrader> tester(new TestTrader());
			TraderAdapterPtr adapter(new TraderAdapter());
			if (!adapter->initExt("trader", tester.get(), &_bd_mgr, NULL))
			{
				WTSLogger::error("Initializing latency test trader adapter failed");
				return false;
			}
			if (!_traders.addAdapter("trader", adapter))
			{
				adapter->release();
				WTSLogger::error("Registering latency test trader adapter failed");
				return false;
			}
			_test_trader_api.reset(tester.release());
		}

		return true;
	}

	void UftLatencyTool::run()
	{
		TestParser* parser = nullptr;
		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			if (_test_parser == nullptr)
			{
				WTSLogger::error("Cannot run UFT latency test before parser initialization succeeds");
				return;
			}
			if (_shutdown_requested.load(std::memory_order_acquire)
				|| _shutdown_complete.load(std::memory_order_acquire))
			{
				WTSLogger::error("Cannot run UFT latency test after shutdown has started");
				return;
			}
			if (_operation_active || _run_attempted)
			{
				WTSLogger::error("UFT latency test run can only be started once");
				return;
			}
			_run_attempted = true;
			_operation_active = true;
			_operation_thread = std::this_thread::get_id();
			parser = _test_parser;
		}

		try
		{
			if (_core != 0)
			{
				if(!CpuHelper::bind_core(_core-1))
				{
					WTSLogger::error("Binding to core {} failed", _core);
				}
			}

			_parsers.run();
			if (!_shutdown_requested.load(std::memory_order_acquire))
				_traders.run();
			if (!_shutdown_requested.load(std::memory_order_acquire))
				_engine.run();
			if (!_shutdown_requested.load(std::memory_order_acquire))
				parser->run(_times);
		}
		catch (...)
		{
			WTSLogger::error("UFT latency test run raised an exception");
		}

		const bool shutdownRequested = _shutdown_requested.load(std::memory_order_acquire);
		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			_operation_active = false;
			_operation_thread = std::thread::id();
		}
		_lifecycle_cv.notify_all();
		if (shutdownRequested)
			shutdown();
	}
}
