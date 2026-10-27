/*!
 * @file UftLatencyTool.h
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief 超高频交易延迟测试工具头文件
 * @details 本文件定义了UFT(超高频交易)延迟测试工具类，用于测量和评估WonderTrader框架中超高频交易模块的内部延迟性能
 */
#pragma once
#include "../WtUftCore/WtUftEngine.h"
#include "../WtUftCore/UftStrategyMgr.h"
#include "../WtUftCore/TraderAdapter.h"
#include "../WtUftCore/ParserAdapter.h"

#include "../WTSTools/WTSBaseDataMgr.h"

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

namespace uft
{
	/**
	 * @brief 超高频交易延迟测试工具类
	 * @details 该类实现了UFT(超高频交易)延迟测试的全流程，包括初始化环境、加载模块、创建策略和执行测试
	 */
	class UftLatencyTool
	{
	public:
		/**
		 * @brief 构造函数
		 */
		UftLatencyTool();

		/**
		 * @brief 析构函数
		 */
		~UftLatencyTool();

	public:
		/**
		 * @brief 初始化延迟测试工具
		 * @return 初始化是否成功
		 * @details 加载配置文件、基础数据文件，并初始化引擎、模块和策略
		 */
		bool init();

		/**
		 * @brief 执行延迟测试
		 * @details 绑定CPU核心，运行解析器、交易器和引擎，并实际执行测试
		 */
		void run();

	private:
		/**
		 * @brief 初始化模块
		 * @return 初始化是否成功
		 * @details 创建并初始化解析器和交易器适配器
		 */
		bool initModules();

		/**
		 * @brief 初始化策略
		 * @return 初始化是否成功
		 * @details 创建测试策略并关联交易器
		 */
		bool initStrategies();

		/**
		 * @brief 初始化引擎
		 * @param cfg 引擎配置
		 * @return 初始化是否成功
		 * @details 初始化UFT引擎并设置适配器管理器
		 */
		bool initEngine(WTSVariant* cfg);

	private:
		TraderAdapterMgr	_traders;    /**< 交易适配器管理器 */
		ParserAdapterMgr	_parsers;    /**< 解析适配器管理器 */
		UftStrategyMgr		_stra_mgr;   /**< 策略管理器 */

		WtUftEngine			_engine;     /**< UFT交易引擎 */

		WTSBaseDataMgr		_bd_mgr;     /**< 基础数据管理器 */

		uint32_t			_times;      /**< 测试模拟的tick数量 */
		uint32_t			_core;       /**< 测试线程绑定的CPU核心编号 */
	};
}
