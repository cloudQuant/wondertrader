/*!
 * \file IndexWorker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 指数计算器定义
 * \details 该文件定义了指数计算器IndexWorker类，负责单个指数的具体计算逻辑，
 *          包括成分合约管理、权重计算以及指数实时生成
 */
#pragma once
#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSStruct.h"
#include "../Includes/FasterDefs.h"

#include "../Share/StdUtils.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/SpinMutex.hpp"

NS_WTP_BEGIN
class WTSVariant;
class WTSTickData;
class IHotMgr;
class IBaseDataMgr;
class WTSContractInfo;
NS_WTP_END

USING_NS_WTP;

class IndexFactory;

/**
 * @brief 指数计算器类
 * @details 负责单个指数的实时计算
 *          管理指数的成分合约及其权重，并根据行情数据动态生成指数值
 *          支持三种算法：固定权重、根据持仓量动态调整权重、根据成交量动态调整权重
 */
class IndexWorker
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化指数计算器对象
	 * @param factor 指数工厂指针，用于访问工厂提供的服务，如热点合约管理器、基础数据管理器等
	 */
	IndexWorker(IndexFactory* factor):_factor(factor), _stopped(false), _process(false) {}

public:
	/**
	 * @brief 初始化指数计算器
	 * @details 根据配置初始化指数计算器，设置指数代码、触发机制、计算参数等
	 *          并订阅指数成分合约的实时行情数据
	 * @param config 配置对象，包含指数的所有配置信息
	 * @return 初始化是否成功
	 */
	bool	init(WTSVariant* config);

	/**
	 * @brief 处理实时行情数据
	 * @details 接收并处理成分合约的新Tick数据，并判断是否触发指数重新计算
	 *          如果触发条件满足，则生成新的指数行情数据
	 * @param newTick 新的Tick数据指针
	 */
	void	handle_quote(WTSTickData* newTick);

private:
	/**
	 * @brief 生成指数Tick数据
	 * @details 根据各成分合约的实时数据和权重计算指数值
	 *          生成指数的Tick数据，并通过指数工厂推送这些数据
	 *          支持三种权重计算算法：固定权重、动态总持、动态成交量
	 */
	void	generate_tick();

protected:
	/// @brief 指数工厂指针，用于访问基础数据管理器和热点合约管理器
	IndexFactory*	_factor;
	/// @brief 指数所属交易所代码
	std::string		_exchg;
	/// @brief 指数代码
	std::string		_code;
	/// @brief 触发条件，可以是合约代码或者"time"表示定时触发
	std::string		_trigger;
	/// @brief 触发超时时间（毫秒），当时间超过这个值后触发指数计算
	uint32_t		_timeout;
	/// @brief 重新计算时间点，用于定时触发时计算下一次触发时间
	uint64_t		_recalc_time;
	/// @brief 标准化系数，用于调整指数值的量级
	double			_stand_scale;
	/// @brief 指数Tick数据缓存，保存当前指数的行情数据
	WTSTickStruct	_cache;
	/// @brief 指数合约信息指针
	WTSContractInfo*	_cInfo;

	/**
	 * @brief 成分合约的权重因子结构
	 * @details 保存每个成分合约的权重和当前的Tick数据
	 */
	typedef struct _WeightFactor
	{
		/// @brief 成分合约的权重值
		double			_weight;
		/// @brief 成分合约的Tick数据
		WTSTickStruct	_tick;
		/**
		 * @brief 构造函数
		 * @details 初始化所有成员为0
		 */
		_WeightFactor()
		{
			memset(this, 0, sizeof(_WeightFactor));
		}
	}WeightFactor;
	/// @brief 数据互斥锁，用于保护成分合约数据的并发访问
	SpinMutex	_mtx_data;
	/// @brief 存储所有成分合约的权重因子数据，以合约完整代码为键
	wt_hashmap<std::string, WeightFactor>	_weight_scales;
	/// @brief 权重算法类型，0=固定权重，1=根据持仓量动态调整，2=根据成交量动态调整
	uint32_t	_weight_alg;

	/// @brief 触发线程指针，用于定时触发指数计算
	StdThreadPtr	_thrd_trigger;
	/// @brief 触发互斥锁，用于保护触发线程中的共享数据
	StdUniqueMutex	_mtx_trigger;
	/// @brief 触发条件变量，用于线程间通知
	StdCondVariable	_cond_trigger;
	/// @brief 是否停止触发线程的标志
	bool			_stopped;
	/// @brief 是否正在处理触发的标志
	bool			_process;
};

typedef std::shared_ptr<IndexWorker> IndexWorkerPtr;