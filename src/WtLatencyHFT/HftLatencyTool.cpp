/*!
 * @file HftLatencyTool.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief 高频交易延迟测试工具实现文件
 * @details 本文件实现了HFT(高频交易)延迟测试的全流程，包括测试解析器、交易器和策略的实现，以及延迟测试工具类的实现
 */
#include "HftLatencyTool.h"
#include "../WtCore/HftStraContext.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSCollection.hpp"
#include "../Includes/IParserApi.h"
#include "../Includes/ITraderApi.h"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"

#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CpuHelper.hpp"

#include <exception>
#include <new>
#ifdef WT_LATENCY_TOOL_TEST_HOOKS
#include <stdexcept>
#endif
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>


USING_NS_WTP;

namespace
{
	// A failed external parser may still call its SPI. In that case retain the
	// complete runner and its borrowed dependencies until process exit.
	hft::HftLatencyTool* g_failed_parser_runner = NULL;
}

/**
 * @brief 测试HFT模块延迟性能
 * @details 创建并初始化HftLatencyTool实例，然后运行高频交易延迟测试流程
 */
bool test_hft()
{
	// Keep the runner heap-owned so a failed parser can quarantine its adapter
	// together with the engine and managers it may still reference.
	hft::HftLatencyTool* runner = new hft::HftLatencyTool();
	// 初始化延迟测试工具
	if (!runner->init())
	{
		if (runner == g_failed_parser_runner)
			return false; // Deliberately retain the quarantined runner until process exit.
		delete runner;
		return false;
	}

	// 执行延迟测试
	runner->run();
	const bool success = !runner->runFailed();
	delete runner;
	return success;
}

namespace hft
{
#ifdef WT_LATENCY_TOOL_TEST_HOOKS
	namespace
	{
		std::atomic<int> g_test_run_failure_stage{ 0 };
		std::atomic<bool> g_test_run_failure_unknown{ false };
		std::atomic<bool> g_test_tick_create_failure{ false };

		WTSTickData* createLatencyTick(const char* code)
		{
			if (g_test_tick_create_failure.exchange(false, std::memory_order_acq_rel))
				return nullptr;
			return WTSTickData::create(code);
		}

		void maybeThrowTestRunFailure(int stage, const char* stageName)
		{
			if (g_test_run_failure_stage.load(std::memory_order_acquire) != stage)
				return;
			if (g_test_run_failure_unknown.load(std::memory_order_acquire))
				throw 42;
			throw std::runtime_error(std::string("injected ") + stageName + " exception");
		}
	}

	void setTestRunFailureForTesting(int stage, bool unknown)
	{
		g_test_run_failure_unknown.store(unknown, std::memory_order_release);
		g_test_run_failure_stage.store(stage, std::memory_order_release);
	}
#define WT_LATENCY_CREATE_TICK(code) createLatencyTick(code)

	void setTestTickCreateFailureForTesting(bool fail)
	{
		g_test_tick_create_failure.store(fail, std::memory_order_release);
	}
#define WT_LATENCY_TEST_THROW(stage, name) maybeThrowTestRunFailure(stage, name)
#else
#define WT_LATENCY_CREATE_TICK(code) WTSTickData::create(code)
#define WT_LATENCY_TEST_THROW(stage, name) ((void)0)
#endif

	/**
	 * @brief 检查浮点数值是否有效
	 * @param x 输入的浮点数值
	 * @return 如果输入为无效值（DBL_MAX或FLT_MAX）则返回0.0，否则返回原值
	 * @details 用于过滤行情数据中的无效值，防止异常数据影响处理逻辑
	 */
	inline double checkValid(double x)
	{
		return ((x == DBL_MAX || x == FLT_MAX) ? 0.0 : x);
	}


	/**
	 * @brief 将时间字符串转换为整数表示
	 * @param strTime 输入的时间字符串，格式如"10:05:23"
	 * @return 时间对应的整数表示，如100523
	 * @details 移除时间字符串中的冒号，并将结果转换为无符号整数
	 */
	inline uint32_t strToTime(const char* strTime)
	{
		static char str[10] = { 0 };
		[[maybe_unused]] const char *pos = strTime;
		int idx = 0;
		auto len = strlen(strTime);
		// 移除字符串中的冒号
		for (std::size_t i = 0; i < len; i++)
		{
			if (strTime[i] != ':')
			{
				str[idx] = strTime[i];
				idx++;
			}
		}
		str[idx] = '\0';

		// 将字符串转换为整数
		return strtoul(str, NULL, 10);
	}

	/**
	 * @brief 测试用行情解析器类
	 * @details 模拟生成行情数据并发送到引擎，用于测量引擎的行情处理延迟
	 */
	class TestParser : public IParserApi
	{
	public:
		virtual bool init(WTSVariant* /*cfg*/) override
		{
			return true;
		}

		bool connect() override
		{
			WT_LATENCY_TEST_THROW(1, "parser");
			return true;
		}

		/**
		 * @brief 执行测试运行
		 * @param times 模拟生成的tick数量
		 * @details 生成指定数量的模拟行情数据，并计算处理全部数据的延迟性能
		 */
		void	run(uint32_t times)
		{
			if (times == 0)
			{
				WTSLogger::warn("0 ticks simulated; average latency unavailable (no samples)");
				return;
			}

			// 初始化随机数生成器
			srand(time(NULL));
			// 创建时间计数器，用于测量总耗时
			TimeUtils::Ticker ticker;
			// 生成指定数量的模拟行情
			for (uint32_t i = 0; i < times; i++)
			{
				// 设置交易日期
				uint32_t actDate = 20220303;// strtoul("20220303", NULL, 10);
				// 设置交易时间，格式为时分秒毫秒
				uint32_t actTime = 100523 * 1000 + 500; //strToTime("10:05:23") * 1000 + 500;

				// 获取铁矿石合约信息
				WTSContractInfo* contract = _bd_mgr->getContract("rb2205", "SHFE");
				if (contract == NULL)
				{
					WTSLogger::warn("Skipping latency test tick: contract {}.{} not found", "SHFE", "rb2205");
					return;
				}

				// 生成随机数作为行情价格
				double x = rand();

				// 获取商品信息
				WTSCommodityInfo* pCommInfo = contract->getCommInfo();
				if (pCommInfo == NULL)
				{
					WTSLogger::warn("Skipping latency test tick: contract {}.{} has no commodity info", "SHFE", "rb2205");
					return;
				}

				// 创建新的tick数据对象
				WTSTickData* tick = WT_LATENCY_CREATE_TICK("rb2205");
				if (tick == NULL)
				{
					WTSLogger::error("HFT latency test failed to create tick SHFE.rb2205");
					throw std::bad_alloc();
				}
				// 设置合约信息
				tick->setContractInfo(contract);

				// 获取tick结构引用，并填充数据
				WTSTickStruct& quote = tick->getTickStruct();
				// 设置交易所代码
				wt_strcpy(quote.exchg, pCommInfo->getExchg());

				// 设置日期和时间
				quote.action_date = actDate;
				quote.action_time = actTime;

				// 设置价格信息
				quote.price = x;      // 当前价
				quote.open = x;       // 开盘价
				quote.high = x;       // 最高价
				quote.low = x;        // 最低价
				quote.total_volume = 0; // 成交量
				quote.trading_date = 20220303; // 交易日期
				quote.settle_price = x; // 结算价

				quote.open_interest = 0; // 持仓量

				// 设置涨跌停板
				quote.upper_limit = x; // 涨停价
				quote.lower_limit = x; // 跌停价

				// 设置前一交易日信息
				quote.pre_close = x;    // 前收盘价
				quote.pre_settle = x;   // 前结算价
				quote.pre_interest = 0; // 前持仓量

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

				// 将tick数据提交给解析器SPI接口
				_parser_spi->handleQuote(tick, 0);
				// 释放tick数据对象
				tick->release();
			}
			// 获取总耗时（纳秒）
			auto total = ticker.nano_seconds();
			// 计算平均每个tick处理时间
			double t2t = total * 1.0 / times;
			// 输出延迟性能测试结果
			WTSLogger::warn("{} ticks simulated in {:.0f} ns, HftEngine Innner Latency: {:.3f} ns", times, total*1.0, t2t);
		}

	public:
		/**
		 * @brief 注册解析器回调接口
		 * @param listener 解析器回调接口对象
		 * @details 实现IParserApi接口的方法，用于注册解析器回调并获取基础数据管理器
		 */
		virtual void registerSpi(IParserSpi* listener) override
		{
			// 保存解析器回调接口
			_parser_spi = listener;
			// 从回调接口获取基础数据管理器
			_bd_mgr = listener->getBaseDataMgr();
		}

	private:
		IParserSpi*		_parser_spi;  /**< 解析器回调接口指针 */
		IBaseDataMgr*	_bd_mgr;      /**< 基础数据管理器指针 */
	};

	/** 全局测试解析器对象指针 */
	TestParser* theParser = NULL;

	namespace
	{
		struct ShutdownGate
		{
			std::mutex mutex;
			std::condition_variable changed;
			WtHftEngine* engine = NULL;
			bool armed = false;
			bool fail_trader_init = false;
			bool entered = false;
			bool release = false;
			bool context_visible = false;
			bool context_visible_after_gate = false;
			bool callback_exited = false;
			bool trader_release_started_during_callback = false;
			bool trader_released_after_callback = false;
		};

		ShutdownGate shutdown_gate;
	}

	// The R95 test uses these controls to hold a real TraderAdapter sink callback
	// while HftLatencyTool tears down its owners.
	void armShutdownGateTest()
	{
		std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
		shutdown_gate.armed = true;
		shutdown_gate.entered = false;
		shutdown_gate.release = false;
		shutdown_gate.context_visible = false;
		shutdown_gate.context_visible_after_gate = false;
		shutdown_gate.callback_exited = false;
		shutdown_gate.trader_release_started_during_callback = false;
		shutdown_gate.trader_released_after_callback = false;
	}

	bool waitForShutdownGateCallback(int timeout_ms)
	{
		std::unique_lock<std::mutex> lock(shutdown_gate.mutex);
		return shutdown_gate.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), []() {
			return shutdown_gate.entered;
		});
	}

	void releaseShutdownGateCallback()
	{
		std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
		shutdown_gate.release = true;
		shutdown_gate.changed.notify_all();
	}

	bool shutdownGateSawContext()
	{
		std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
		return shutdown_gate.context_visible && shutdown_gate.context_visible_after_gate;
	}

	bool shutdownGateSawTraderReleaseAfterCallback()
	{
		std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
		return shutdown_gate.trader_released_after_callback;
	}

	bool waitForTraderReleaseDuringCallback(int timeout_ms)
	{
		std::unique_lock<std::mutex> lock(shutdown_gate.mutex);
		return shutdown_gate.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), []() {
			return shutdown_gate.trader_release_started_during_callback;
		});
	}

	bool shutdownGateSawTraderReleaseDuringCallback()
	{
		std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
		return shutdown_gate.trader_release_started_during_callback;
	}

	void disarmShutdownGateTest()
	{
		std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
		shutdown_gate.armed = false;
		shutdown_gate.engine = NULL;
		shutdown_gate.fail_trader_init = false;
	}

	bool shutdownGateIsArmed()
	{
		std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
		return shutdown_gate.armed;
	}

	void setTestTraderInitFailure(bool fail)
	{
		std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
		shutdown_gate.fail_trader_init = fail;
	}

	/**
	 * @brief 测试用交易器类
	 * @details 模拟的交易器实现，用于测试高频交易时的交易处理流程
	 */
	class TestTrader : public ITraderApi
	{
	public:
		bool init(WTSVariant* /*cfg*/) override
		{
			std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
			return !shutdown_gate.fail_trader_init;
		}

		void connect() override
		{
			WT_LATENCY_TEST_THROW(2, "trader");
			if (!shutdownGateIsArmed())
				return;

			ITraderSpi* spi = _trader_spi;
			_connection_thread = std::thread([spi]() {
				if (spi != NULL)
					spi->onLoginResult(true, "test", 20220303);
			});
		}

		void release() override
		{
			{
				std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
				if (shutdown_gate.armed && !shutdown_gate.callback_exited)
				{
					shutdown_gate.trader_release_started_during_callback = true;
					shutdown_gate.changed.notify_all();
				}
			}
			if (_connection_thread.joinable())
				_connection_thread.join();
			std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
			if (shutdown_gate.armed)
				shutdown_gate.trader_released_after_callback = shutdown_gate.callback_exited;
		}

		int queryPositions() override
		{
			WTSArray* empty = WTSArray::create();
			if (_trader_spi != NULL)
				_trader_spi->onRspPosition(empty);
			empty->release();
			return 0;
		}

		int queryOrders() override
		{
			WTSArray* empty = WTSArray::create();
			if (_trader_spi != NULL)
				_trader_spi->onRspOrders(empty);
			empty->release();
			return 0;
		}

		int queryTrades() override
		{
			WTSArray* empty = WTSArray::create();
			if (_trader_spi != NULL)
				_trader_spi->onRspTrades(empty);
			empty->release();
			return 0;
		}

		int queryAccount() override
		{
			WTSArray* empty = WTSArray::create();
			if (_trader_spi != NULL)
				_trader_spi->onRspAccount(empty);
			empty->release();
			return 0;
		}

		/**
		 * @brief 注册交易器回调接口
		 * @param listener 交易器回调接口对象
		 * @details 实现ITraderApi接口的方法，用于注册交易器回调接口
		 */
		virtual void registerSpi(ITraderSpi* listener) override
		{
			// 保存交易器回调接口
			_trader_spi = listener;
		}

		/**
		 * @brief 生成委托单号
		 * @param buffer 存放生成的委托单号的缓冲区
		 * @param length 缓冲区长度
		 * @return 委托单号生成是否成功
		 * @details 实现ITraderApi接口的方法，生成简单固定的委托单号
		 */
		virtual bool makeEntrustID(char* buffer, int /*length*/) override
		{
			// 生成固定的委托单号（测试用途）
			wt_strcpy(buffer, "123456");
			return true;
		}

		/**
		 * @brief 插入订单
		 * @param eutrust 委托单对象
		 * @return 返回码，0表示成功
		 * @details 实现ITraderApi接口的方法，处理委托单插入请求，在测试中只返回成功状态
		 */
		virtual int orderInsert(WTSEntrust* /*eutrust*/) override
		{
			// 测试环境下直接返回成功
			return 0;
		}

	private:
		ITraderSpi*	_trader_spi = NULL;  /**< 交易器回调接口指针 */
		std::thread _connection_thread;
	};

	/**
	 * @brief 测试用高频策略类
	 * @details 模拟的高频交易策略实现，用于测试高频交易引擎的延迟性能
	 */
	class TestStrategy : public HftStrategy
	{
	public:
		/**
		 * @brief 构造函数
		 * @param id 策略唯一标识
		 * @details 创建测试策略实例，并调用父类构造函数
		 */
		TestStrategy(const char* id) : HftStrategy(id) {}

		/**
		 * @brief 获取策略名称
		 * @return 策略名称
		 * @details 返回测试策略的名称，用于标识策略
		 */
		virtual const char* getName() override { return "TestStrategy"; }

		/**
		 * @brief 获取策略所属工厂名称
		 * @return 策略工厂名称
		 * @details 返回测试策略所属的工厂名称，用于策略创建和管理
		 */
		virtual const char* getFactName() override { return "TestStrategyFact"; }


		/**
		 * @brief 策略初始化回调
		 * @param ctx 策略上下文
		 * @details 在策略初始化时调用，订阅铁矿石合约的行情数据
		 */
		virtual void on_init(IHftStraCtx* ctx) override
		{
			WT_LATENCY_TEST_THROW(3, "engine");
			// 订阅铁矿石2205合约的tick数据
			ctx->stra_sub_ticks("SHFE.rb.2205");
		}

		/**
		 * @brief 行情数据Tick回调
		 * @param ctx 策略上下文
		 * @param code 合约代码
		 * @param newTick 新到达的Tick数据
		 * @details 当收到新的Tick数据时调用，执行买入操作，用于测试高频交易引擎的冲击
		 */
		virtual void on_tick(IHftStraCtx* ctx, const char* /*code*/, WTSTickData* /*newTick*/) override
		{
			// 注释掉的卖出代码
			//ctx->stra_sell("SHFE.rb.2205", 2300, 1, "", HFT_OrderFlag_Nor);
			// 买入铁矿石2205合约，价格2300，数量1手，测试高频交易引擎的响应速度
			ctx->stra_buy("SHFE.rb.2205", 2300, 1, "", HFT_OrderFlag_Nor);
		}

		void on_channel_ready(IHftStraCtx* ctx) override
		{
			std::unique_lock<std::mutex> lock(shutdown_gate.mutex);
			if (!shutdown_gate.armed)
				return;
			WtHftEngine* engine = shutdown_gate.engine;
			lock.unlock();
			bool context_visible = engine != NULL && engine->getContext(ctx->id()) != NULL;
			lock.lock();
			shutdown_gate.context_visible = context_visible;
			shutdown_gate.entered = true;
			shutdown_gate.changed.notify_all();
			shutdown_gate.changed.wait(lock, []() { return shutdown_gate.release; });
			lock.unlock();

			bool context_still_visible = engine != NULL && engine->getContext(ctx->id()) != NULL;
			lock.lock();
			shutdown_gate.context_visible_after_gate = context_still_visible;
			shutdown_gate.callback_exited = true;
			shutdown_gate.changed.notify_all();
		}
	};


	/**
	 * @brief 高频交易延迟测试工具类的构造函数
	 * @details 创建一个新的高频交易延迟测试工具实例
	 */
	HftLatencyTool::HftLatencyTool()
	{
		theParser = NULL;
		_init_attempted = false;
		_shutdown_complete = false;
	}


	/**
	 * @brief 高频交易延迟测试工具类的析构函数
	 * @details 清理高频交易延迟测试工具的资源
	 */
	HftLatencyTool::~HftLatencyTool()
	{
		if (!shutdown())
		{
			WTSLogger::error("HFT latency tool cannot be destroyed from a trader sink callback");
			std::terminate();
		}
	}

	bool HftLatencyTool::shutdown()
	{
		if (_shutdown_complete)
			return true;

		const TraderAdapterMap& adapters = _traders.getAdapters();
		for (const auto& item : adapters)
			item.second->requestStopSinkNotifications();

		// Stop notification producers while the engine still owns every strategy context.
		_parsers.release();
		theParser = NULL;
		_engine.release();

		bool drained = true;
		for (const auto& item : adapters)
		{
			if (!item.second->waitSinkNotificationsStopped())
				drained = false;
		}
		if (!drained)
			return false;

		_traders.release();
		_shutdown_complete = true;
		return true;
	}

	/**
	 * @brief 初始化高频交易延迟测试工具
	 * @return 初始化是否成功
	 * @details 加载配置文件、基础数据、初始化引擎和模块，为测试做准备
	 */
	bool HftLatencyTool::init()
	{
		// Reject reinitialization without mutating an already valid parser state.
		if (_init_attempted)
		{
			WTSLogger::error("HFT latency tool initialization can only be attempted once");
			return false;
		}
		_init_attempted = true;
		theParser = NULL;

		// 初始化日志系统
		WTSLogger::init("logcfg.yaml");

		// 加载配置文件
		WTSVariant* _config = WTSCfgLoader::load_from_file("config.yaml");
		if (_config == NULL)
		{
			WTSLogger::log_raw(LL_ERROR, "Loading config file config.yaml failed");
			return false;
		}

		//基础数据文件
		WTSVariant* cfgBF = _config->get("basefiles");
		if (cfgBF == NULL || !cfgBF->isObject())
		{
			WTSLogger::log_raw(LL_ERROR, cfgBF == NULL
				? "Invalid config: required basefiles object is missing"
				: "Invalid config: basefiles must be an object");
			_config->release();
			return false;
		}
		// Child paths are optional; only the basefiles container itself is required.
		[[maybe_unused]] bool isUTF8 = cfgBF->getBoolean("utf-8");
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

		if (cfgBF->get("hot"))
		{
			_hot_mgr.loadHots(cfgBF->getCString("hot"));
			WTSLogger::log_raw(LL_INFO, "Hot rules loades");
		}

		// 初始化行动策略管理器
		_act_mgr.init("actpolicy.yaml");

		_times = _config->getUInt32("times");
		WTSLogger::warn("{} ticks will be simulated", _times);

		_core = _config->getUInt32("core");
		WTSLogger::warn("Testing thread will be bind to core {}", _core);

		initEngine(_config->get("env"));
		if (!initModules() || !initStrategies())
		{
			if (g_failed_parser_runner == this)
				WTSLogger::error("Latency parser failed initialization; retaining its adapter and borrowed owner graph until process exit");
			else if (!shutdown())
				WTSLogger::error("HFT latency initialization cleanup is pending until trader callbacks return");
			_config->release();
			return false;
		}

		_config->release();
		return true;
	}

	/**
	 * @brief 初始化高频交易策略
	 * @return 初始化是否成功
	 * @details 创建高频交易策略上下文，设置测试策略，并将其添加到引擎中
	 */
	bool HftLatencyTool::initStrategies()
	{
		HftStraContext* ctx = new HftStraContext(&_engine, "stra", false, 0);
		ctx->set_strategy(new TestStrategy("stra"));

		TraderAdapterPtr trader = _traders.getAdapter("trader");
		if (!trader)
		{
			WTSLogger::error("Latency test trader adapter is unavailable");
			return false;
		}
		ctx->setTrader(trader.get());
		_engine.addContext(HftContextPtr(ctx));
		{
			std::lock_guard<std::mutex> lock(shutdown_gate.mutex);
			shutdown_gate.engine = &_engine;
		}
		// addSink may synchronously deliver READY when the adapter is already connected.
		// Publish the fully initialized context first so the engine can resolve callbacks.
		trader->addSink(ctx);

		return true;
	}

	/**
	 * @brief 初始化高频交易引擎
	 * @param cfg 引擎配置对象
	 * @return 初始化是否成功
	 * @details 初始化高频交易引擎，设置相关的数据管理器和适配器管理器
	 */
	bool HftLatencyTool::initEngine(WTSVariant* cfg)
	{
		WTSLogger::warn("Trading enviroment initialzied with engine: HFT");
		_engine.init(cfg, &_bd_mgr, &_dt_mgr, &_hot_mgr, NULL);
		if (!_engine.fee_config_valid())
		{
			WTSLogger::error("Latency test engine initialization rejected: fee template configuration is invalid");
			return false;
		}
		_engine.set_adapter_mgr(&_traders);

		return true;
	}


	/**
	 * @brief 初始化模块
	 * @return 初始化是否成功
	 * @details 初始化解析器和交易器模块，并将它们添加到相应的适配器管理器中
	 */
	bool HftLatencyTool::initModules()
	{
		{
			TestParser* parser = new TestParser();
			ParserAdapterPtr adapter(new ParserAdapter);
			// Register before ownership transfer so a failed initExt result cannot
			// destroy the adapter while the provider may still hold its SPI pointer.
			if (!_parsers.addAdapter("parser", adapter))
			{
				delete parser;
				WTSLogger::error("Registering latency test parser adapter failed");
				return false;
			}

			bool initialized = false;
			try
			{
				initialized = adapter->initExt("parser", parser, &_engine, &_bd_mgr, &_hot_mgr);
			}
			catch (...)
			{
				initialized = false;
			}
			if (!initialized)
			{
				// initExt takes ownership as soon as this non-null API is passed.
				// Keep the entire runner alive: release() is not a callback drain.
				g_failed_parser_runner = this;
				WTSLogger::error("Initializing latency test parser adapter failed; owner graph quarantined");
				return false;
			}
			theParser = parser;
		}

		{
			std::unique_ptr<ITraderApi> tester(new TestTrader());
			TraderAdapterPtr adapter(new TraderAdapter());
			if (!adapter->initExt("trader", tester.get(), &_bd_mgr, &_act_mgr))
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
			_test_trader_api = std::move(tester);
		}

		return true;
	}

	/**
	 * @brief 运行高频交易延迟测试
	 * @details 启动解析器、交易器和引擎，绑定CPU核心，然后执行测试运行
	 */
	void HftLatencyTool::run()
	{
		_run_failed.store(true, std::memory_order_release);
		if (theParser == NULL)
		{
			WTSLogger::error("Cannot run HFT latency test before parser initialization succeeds");
			return;
		}

		if (_core != 0)
		{
			if (!CpuHelper::bind_core(_core - 1))
			{
				WTSLogger::error("Binding to core {} failed", _core);
			}
		}

		try
		{
			_parsers.run();
			_traders.run();

			_engine.run();

			theParser->run(_times);
			_run_failed.store(false, std::memory_order_release);
		}
		catch (const std::exception& e)
		{
			_run_failed.store(true, std::memory_order_release);
			WTSLogger::error("HFT latency test run failed with exception: {}", e.what());
		}
		catch (...)
		{
			_run_failed.store(true, std::memory_order_release);
			WTSLogger::error("HFT latency test run failed with unknown exception");
		}
	}
}
#undef WT_LATENCY_TEST_THROW
#undef WT_LATENCY_CREATE_TICK
