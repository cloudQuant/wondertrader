/*!  
 * \file WtSelEngine.h
 * \project	WonderTrader
 *
 * \author Wesley
 * 
 * \brief 选股引擎头文件
 * \details 定义了选股策略的引擎类，负责管理选股策略的生命周期、数据分发、
 *          定时任务处理和执行器管理
 */

#pragma once
#include "WtEngine.h"
#include "WtExecMgr.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/ISelStraCtx.h"

#include <memory>

NS_WTP_BEGIN

/**
 * @brief 选股策略定时任务周期类型
 * @details 定义了各种周期类型的计划任务，用于控制策略执行频率
 */
typedef enum tagTaskPeriodType
{
	TPT_None,		///<不重复，只执行一次
	TPT_Minute = 4,	///<分钟周期，每分钟执行一次
	TPT_Daily = 8,	///<每个交易日执行一次
	TPT_Weekly,		///<每周执行一次，遇到节假日的话要顺延
	TPT_Monthly,	///<每月执行一次，遇到节假日的话要顺延
	TPT_Yearly		///<每年执行一次，遇到节假日的话要顺延
}TaskPeriodType;

/**
 * @brief 选股策略定时任务信息结构体
 * @details 定义了选股策略的定时任务结构，包括任务ID、名称、时间设置、周期类型等信息
 */
typedef struct _TaskInfo
{
	uint32_t	_id;			///<任务唯一标识编号
	char		_name[16];		///<任务名称
	char		_trdtpl[16];	///<交易日模板，用于确定交易日
	char		_session[16];	///<交易时间模板，用于确定交易时段
	uint32_t	_day;			///<日期参数，根据周期类型变化：每日为0，每周为0~6（对应周日到周六），每月为1~31，每年为0101~1231
	uint32_t	_time;			///<触发时间，精确到分钟，格式HHMM
	bool		_strict_time;	///<是否严格执行时间：严格模式下只有时间相等才会执行，非严格模式下只要超过设定时间就会执行

	uint64_t	_last_exe_time;	///<上次执行时间，用于防止重复执行

	TaskPeriodType	_period;	///<任务周期类型，参考TaskPeriodType定义
} TaskInfo;

/**
 * @brief 选股任务信息智能指针类型
 * @details 使用智能指针管理TaskInfo对象，自动管理内存生命周期
 */
typedef std::shared_ptr<TaskInfo> TaskInfoPtr;

/**
 * @brief 选股策略上下文智能指针类型
 * @details 使用智能指针管理ISelStraCtx接口对象，自动管理内存生命周期
 */
typedef std::shared_ptr<ISelStraCtx> SelContextPtr;

/**
 * @brief 选股实时定时器类前向声明
 * @details 用于管理选股策略的定时执行
 */
class WtSelRtTicker;


/**
 * @brief 选股策略引擎类
 * @details 负责选股策略的管理、数据分发、定时执行和信号生成。
 *          继承WtEngine进行基础的行情数据处理，并实现IExecuterStub接口与执行器进行交互。
 *          选股策略通常是周期性执行，通过TaskInfo管理执行时间和周期
 */
class WtSelEngine : public WtEngine, public IExecuterStub
{
public:
	/**
	 * @brief 选股引擎类构造函数
	 * @details 初始化选股引擎对象，初始化内部成员变量
	 */
	WtSelEngine();

	/**
	 * @brief 选股引擎类析构函数
	 * @details 清理选股引擎对象相关资源
	 */
	~WtSelEngine();

public:
	//////////////////////////////////////////////////////////////////////////
	//WtEngine接口
	/**
	 * @brief 初始化选股引擎
	 * @param cfg 配置项
	 * @param bdMgr 基础数据管理器
	 * @param dataMgr 数据管理器
	 * @param hotMgr 主力合约管理器
	 * @param notifier 事件通知器
	 * @details 初始化选股引擎相关组件和参数设置
	 */
	virtual void init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier) override;

	/**
	 * @brief 运行选股引擎
	 * @details 启动引擎并进入工作状态
	 */
	virtual void run() override;

	/**
	 * @brief Tick数据回调
	 * @param stdCode 标准化合约代码
	 * @param curTick 当前市场行情数据
	 * @details 接收并处理Tick数据，分发给策略
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* curTick) override;

	/**
	 * @brief K线数据回调
	 * @param stdCode 标准化合约代码
	 * @param period 周期标识
	 * @param times 周期倍数
	 * @param newBar 新K线数据
	 * @details 接收并处理K线数据，分发给策略
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 处理推送行情
	 * @param newTick 新行情数据
	 * @details 处理外部推送的实时行情数据
	 */
	virtual void handle_push_quote(WTSTickData* newTick) override;

	/**
	 * @brief 引擎初始化完成事件
	 * @details 引擎启动后执行的初始化回调函数
	 */
	virtual void on_init() override;

	/**
	 * @brief 交易会话开始事件
	 * @details 每个交易日开盘时执行的回调函数
	 */
	virtual void on_session_begin() override;

	/**
	 * @brief 交易会话结束事件
	 * @details 每个交易日收盘时执行的回调函数
	 */
	virtual void on_session_end() override;

	///////////////////////////////////////////////////////////////////////////
	//IExecuterStub 接口
	/**
	 * @brief 获取当前实时时间
	 * @return 返回当前时间的数值表示，精确到毫秒
	 * @details 提供给执行器的时间戳获取接口
	 */
	virtual uint64_t get_real_time() override;

	/**
	 * @brief 获取商品信息
	 * @param stdCode 标准化合约代码
	 * @return 商品信息对象指针
	 * @details 获取指定合约的基础商品信息，如商品代码、名称、基础单位等
	 */
	virtual WTSCommodityInfo* get_comm_info(const char* stdCode) override;

	/**
	 * @brief 获取交易时段信息
	 * @param stdCode 标准化合约代码
	 * @return 交易时段信息对象指针
	 * @details 获取指定合约的交易时段信息，如开盘时间、收盘时间等
	 */
	virtual WTSSessionInfo* get_sess_info(const char* stdCode) override;

	/**
	 * @brief 获取主力合约管理器
	 * @return 主力合约管理器指针
	 * @details 获取用于管理主力合约映射的管理器指针
	 */
	virtual IHotMgr* get_hot_mon() { return _hot_mgr; }

	/**
	 * @brief 获取当前交易日
	 * @return 交易日，格式YYYYMMDD
	 * @details 获取系统当前交易日期
	 */
	virtual uint32_t get_trading_day() { return _cur_tdate; }

public:
	//uint32_t	register_task(const char* name, uint32_t date, uint32_t time, TaskPeriodType period, bool bStrict = true, const char* trdtpl = "CHINA");

	/**
	 * @brief 添加选股策略上下文
	 * @param ctx 选股策略上下文指针
	 * @param date 执行日期参数，根据周期类型有不同含义
	 * @param time 执行时间，格式HHMM
	 * @param period 执行周期类型
	 * @param bStrict 是否严格时间模式
	 * @param trdtpl 交易日模板名称
	 * @param sessionID 交易时段ID
	 * @details 添加并注册一个选股策略到引擎中，并设置其定时执行参数
	 */
	void			addContext(SelContextPtr ctx, uint32_t date, uint32_t time, TaskPeriodType period, bool bStrict = true, const char* trdtpl = "CHINA", const char* sessionID="TRADING");

	/**
	 * @brief 获取指定ID的选股策略上下文
	 * @param id 策略ID
	 * @return 选股策略上下文指针
	 * @details 根据ID查找并返回已注册的选股策略上下文
	 */
	SelContextPtr	getContext(uint32_t id);

	/**
	 * @brief 添加执行器
	 * @param executer 执行器指针
	 * @details 添加一个交易指令执行器，并设置其执行态枥对象为当前引擎
	 */
	inline void addExecuter(ExecCmdPtr& executer)
	{
		_exec_mgr.add_executer(executer);
		executer->setStub(this);
	}

	/**
	 * @brief 分钟结束回调
	 * @param uDate 日期，格式YYYYMMDD
	 * @param uTime 时间，格式HHMMSS
	 * @details 每分钟结束时调用，用于检查并触发需要在当前时间执行的任务
	 */
	void	on_minute_end(uint32_t uDate, uint32_t uTime);

	/**
	 * @brief 处理持仓变化
	 * @param straName 策略名称
	 * @param stdCode 标准化合约代码
	 * @param diffQty 变化数量
	 * @details 处理策略生成的持仓变化信号，转发给执行器进行交易
	 */
	void	handle_pos_change(const char* straName, const char* stdCode, double diffQty);

private:
	/**
	 * @brief 任务映射表
	 * @details 存储所有注册的任务信息，以任务ID为键
	 */
	wt_hashmap<uint32_t, TaskInfoPtr>	_tasks;

	/**
	 * @brief 选股策略上下文映射表类型定义
	 * @details 定义了策略ID到选股策略上下文的映射表类型
	 */
	typedef wt_hashmap<uint32_t, SelContextPtr> ContextMap;

	/**
	 * @brief 选股策略上下文映射表
	 * @details 存储所有注册的选股策略上下文，以策略ID为键
	 */
	ContextMap		_ctx_map;

	/**
	 * @brief 执行器管理器
	 * @details 管理所有注册的交易指令执行器
	 */
	WtExecuterMgr	_exec_mgr;

	/**
	 * @brief 终止标志
	 * @details 标记引擎是否已终止运行
	 */
	bool	_terminated;

	/**
	 * @brief 选股实时定时器指针
	 * @details 用于管理选股策略的定时执行
	 */
	WtSelRtTicker*	_tm_ticker;

	/**
	 * @brief 引擎配置对象
	 * @details 存储引擎的配置参数
	 */
	WTSVariant*		_cfg;
};

NS_WTP_END
