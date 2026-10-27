/*!
 * \file ExecMocker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 执行单元模拟器头文件，用于回测过程中模拟执行交易指令
 * \details 该文件定义了ExecMocker类，用于在回测环境中模拟交易执行器的行为，
 *          实现订单管理、交易撮合以及实时持仓的维护等功能
 */
#pragma once
#include <sstream>
#include "HisDataReplayer.h"

#include "../Includes/ExecuteDefs.h"
#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "MatchEngine.h"

USING_NS_WTP;

/**
 * @brief 执行单元模拟器类
 * @details 继承自ExecuteContext、IDataSink和IMatchSink接口，
 *          用于在回测环境中模拟交易执行单元的行为，
 *          接收策略发出的交易指令，通过内部撮合引擎进行处理，
 *          并将结果通过回调方式通知给策略
 */
class ExecMocker : public ExecuteContext, public IDataSink, public IMatchSink
{
public:
    /**
     * @brief 构造函数
     * @param replayer 历史数据回放器指针
     */
	ExecMocker(HisDataReplayer* replayer);
    
    /**
     * @brief 析构函数
     */
	virtual ~ExecMocker();

public:
	//////////////////////////////////////////////////////////////////////////
	//IMatchSink
	/**
	 * @brief 处理成交回调
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否买入
	 * @param vol 成交数量
	 * @param fireprice 委托价格
	 * @param price 成交价格
	 * @param ordTime 委托时间
	 * @details 撮合引擎完成撮合后会调用此接口，通知成交结果
	 */
	virtual void handle_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double fireprice, double price, uint64_t ordTime) override;
	
	/**
	 * @brief 处理订单状态变更回调
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否买入
	 * @param leftover 剩余未成交数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤单
	 * @param ordTime 委托时间
	 * @details 订单状态变更时调用，如部分成交、已撤单等
	 */
	virtual void handle_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled, uint64_t ordTime) override;
	
	/**
	 * @brief 处理委托回报回调
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 委托回报消息
	 * @param ordTime 委托时间
	 * @details 委托发出后返回的状态回报，包含成功与失败的信息
	 */
	virtual void handle_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message, uint64_t ordTime) override;

	//////////////////////////////////////////////////////////////////////////
	//IDataSink
	/**
	 * @brief 处理逆实时行情数据
	 * @param stdCode 标准化合约代码
	 * @param curTick 当前行情数据
	 * @param pxType 价格类型
	 * @details 收到逆实时行情数据时调用，用于处理tick数据并触发相应的撮合逻辑
	 */
	virtual void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override;
	
	/**
	 * @brief 处理定时任务
	 * @param uDate 当前日期(YYYYMMDD)
	 * @param uTime 当前时间(HHMMSS)
	 * @details 回测引擎定时触发的任务，用于执行定时的适配和操作
	 */
	virtual void handle_schedule(uint32_t uDate, uint32_t uTime) override;
	
	/**
	 * @brief 初始化处理
	 * @details 回测引擎启动时调用，用于执行执行单元的初始化操作
	 */
	virtual void handle_init() override;

	/**
	 * @brief 处理K线周期结束
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param times 周期倍数
	 * @param newBar 新生成的K线
	 * @details K线周期结束时调用，用于处理周期性的任务
	 */
	virtual void handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 处理交易日开始
	 * @param curTDate 当前交易日(YYYYMMDD)
	 * @details 每个交易日开始时调用，用于处理日初始化任务
	 */
	virtual void handle_session_begin(uint32_t curTDate) override;

	/**
	 * @brief 处理交易日结束
	 * @param curTDate 当前交易日(YYYYMMDD)
	 * @details 每个交易日结束时调用，用于清理撮合引擎并输出统计信息
	 */
	virtual void handle_session_end(uint32_t curTDate) override;

	/**
	 * @brief 处理回放结束
	 * @details 回测回放全部完成后调用，用于输出交易日志和统计结果
	 */
	virtual void handle_replay_done() override;

	//////////////////////////////////////////////////////////////////////////
	//ExecuteContext
	/**
	 * @brief 获取指定数量的历史Tick数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 获取的Tick数量
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return WTSTickSlice* Tick切片数据指针
	 * @details 从历史数据中获取指定数量的Tick数据，用于策略分析
	 */
	virtual WTSTickSlice* getTicks(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取最新的Tick数据
	 * @param stdCode 标准化合约代码
	 * @return WTSTickData* Tick数据指针
	 * @details 获取当前最新的市场行情数据
	 */
	virtual WTSTickData* grabLastTick(const char* stdCode) override;

	/**
	 * @brief 获取当前持仓
	 * @param stdCode 标准化合约代码
	 * @param validOnly 是否只考虑有效持仓
	 * @param flag 持仓标记，默认为3(全部)，1(多头持仓)或2(空头持仓)
	 * @return double 持仓量，正数表示多头持仓，负数表示空头持仓
	 * @details 获取当前合约的持仓量，可以根据参数指定只获取特定方向的持仓
	 */
	virtual double getPosition(const char* stdCode, bool validOnly = true, int32_t flag = 3) override;

	/**
	 * @brief 获取当前所有订单
	 * @param stdCode 标准化合约代码
	 * @return OrderMap* 订单映射对象指针
	 * @details 获取指定合约的当前所有未完成订单
	 */
	virtual OrderMap* getOrders(const char* stdCode) override;

	/**
	 * @brief 获取未完成数量
	 * @param stdCode 标准化合约代码
	 * @return double 未完成数量，正数表示多头未完成，负数表示空头未完成
	 * @details 获取指定合约的所有未完成委托数量
	 */
	virtual double getUndoneQty(const char* stdCode) override;

	/**
	 * @brief 执行买入操作
	 * @param stdCode 标准化合约代码
	 * @param price 委托价格
	 * @param qty 委托数量
	 * @param bForceClose 是否强制平仓
	 * @return OrderIDs 订单ID集合
	 * @details 发起买入操作，返回创建的订单ID集合
	 */
	virtual OrderIDs buy(const char* stdCode, double price, double qty, bool bForceClose = false) override;

	/**
	 * @brief 执行卖出操作
	 * @param stdCode 标准化合约代码
	 * @param price 委托价格
	 * @param qty 委托数量
	 * @param bForceClose 是否强制平仓
	 * @return OrderIDs 订单ID集合
	 * @details 发起卖出操作，返回创建的订单ID集合
	 */
	virtual OrderIDs sell(const char* stdCode, double price, double qty, bool bForceClose = false) override;

	/**
	 * @brief 根据订单ID撤单
	 * @param localid 本地订单ID
	 * @return bool 是否撤单成功
	 * @details 通过指定订单ID撤销特定订单
	 */
	virtual bool cancel(uint32_t localid) override;

	/**
	 * @brief 根据合约和方向批量撤单
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否买入方向
	 * @param qty 要撤销的数量，默认为0表示全部撤销
	 * @return OrderIDs 被撤销的订单ID集合
	 * @details 批量撤销指定合约和方向的订单
	 */
	virtual OrderIDs cancel(const char* stdCode, bool isBuy, double qty = 0) override;

	/**
	 * @brief 写入日志
	 * @param message 日志消息
	 * @details 将指定消息写入日志系统
	 */
	virtual void writeLog(const char* message) override;

	/**
	 * @brief 获取商品信息
	 * @param stdCode 标准化合约代码
	 * @return WTSCommodityInfo* 商品信息指针
	 * @details 获取指定合约的商品基础信息
	 */
	virtual WTSCommodityInfo* getCommodityInfo(const char* stdCode) override;
	
	/**
	 * @brief 获取交易时间段信息
	 * @param stdCode 标准化合约代码
	 * @return WTSSessionInfo* 交易时段信息指针
	 * @details 获取指定合约的交易时间段信息
	 */
	virtual WTSSessionInfo* getSessionInfo(const char* stdCode) override;

	/**
	 * @brief 获取当前时间
	 * @return uint64_t 当前时间戳
	 * @details 获取当前回测时间点的时间戳
	 */
	virtual uint64_t getCurTime() override;

public:
	/**
	 * @brief 初始化执行单元模拟器
	 * @param cfg 配置参数对象
	 * @return bool 初始化是否成功
	 * @details 根据配置参数初始化执行单元模拟器，加载执行单元模块并设置其参数
	 */
	bool	init(WTSVariant* cfg);

private:
	HisDataReplayer*	_replayer;    ///< 历史数据回放器指针

	/**
	 * @brief 执行器工厂信息结构体
	 * @details 保存执行器工厂的动态库和函数指针信息
	 */
	typedef struct _ExecFactInfo
	{
		std::string		_module_path;    ///< 模块路径
		DllHandle		_module_inst;    ///< 模块句柄
		IExecuterFact*	_fact;          ///< 执行器工厂实例
		FuncCreateExeFact	_creator;   ///< 创建执行器工厂的函数指针
		FuncDeleteExeFact	_remover;   ///< 删除执行器工厂的函数指针

		/**
		 * @brief 构造函数
		 * @details 初始化成员变量为空值
		 */
		_ExecFactInfo()
		{
			_module_inst = NULL;
			_fact = NULL;
		}

		/**
		 * @brief 析构函数
		 * @details 释放执行器工厂实例
		 */
		~_ExecFactInfo()
		{
			if (_fact)
				_remover(_fact);
		}
	} ExecFactInfo;
	ExecFactInfo	_factory;           ///< 执行器工厂实例

	ExecuteUnit*	_exec_unit;         ///< 执行单元实例
	std::string		_code;            ///< 合约代码
	std::string		_period;          ///< K线周期
	double			_volunit;           ///< 数量单位
	int32_t			_volmode;           ///< 数量模式：0-反复正负，-1-一直卖，+1-一直买

	double			_target;            ///< 目标仓位

	double			_position;          ///< 当前持仓
	double			_undone;            ///< 未完成数量
	WTSTickData*	_last_tick;         ///< 最新的Tick数据
	double			_sig_px;            ///< 信号价格
	uint64_t		_sig_time;          ///< 信号时间

	std::stringstream	_trade_logs;    ///< 交易日志流
	uint32_t	_ord_cnt;              ///< 委托计数
	double		_ord_qty;              ///< 委托总量
	uint32_t	_cacl_cnt;             ///< 撤单计数
	double		_cacl_qty;             ///< 撤单总量
	uint32_t	_sig_cnt;              ///< 信号计数

	std::string	_id;                ///< 执行单元ID

	MatchEngine	_matcher;           ///< 撮合引擎
};

