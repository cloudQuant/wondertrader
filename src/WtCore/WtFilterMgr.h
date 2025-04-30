/**
 * @file WtFilterMgr.h
 * @author wondertrader
 * @brief 信号过滤管理器头文件
 * @details 定义了信号过滤管理器类，用于过滤和处理交易信号
 */

#pragma once
#include <string>
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN

class EventNotifier;

/**
 * @brief 信号过滤管理器类
 * @details 管理交易信号的过滤规则，包括策略过滤器、合约代码过滤器和执行器过滤器
 */
class WtFilterMgr
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化过滤器时间戳和通知器
	 */
	WtFilterMgr():_filter_timestamp(0), _notifier(NULL){}

	/**
	 * @brief 设置事件通知器
	 * @details 设置过滤器事件的通知器
	 * @param notifier 事件通知器指针
	 */
	void		set_notifier(EventNotifier* notifier) { _notifier = notifier; }

	/**
	 * @brief 加载信号过滤器
	 * @details 从指定文件加载信号过滤器的配置
	 * @param fileName 过滤器配置文件名，默认为空字符串
	 */
	void		load_filters(const char* fileName = "");

	/**
	 * @brief 检查是否因为策略被过滤掉了
	 * @details 检查策略信号是否被过滤器过滤掉了。如果过滤器动作是忽略，则返回true，
	 *          如果是重定向仓位，则返回false，同时修改目标仓位为过滤器设定的值
	 * @param straName 策略名称
	 * @param targetPos 目标仓位，传入参数，会被修改
	 * @param isDiff 是否是增量仓位，默认为false
	 * @return 是否过滤掉了，如果过滤掉了，该持仓就不加入最终组合目标仓位
	 */
	bool		is_filtered_by_strategy(const char* straName, double& targetPos, bool isDiff = false);

	/**
	 * @brief 检查是否因为合约代码被过滤掉了
	 * @details 检查合约代码是否被过滤器过滤掉了。如果过滤器动作是忽略，则返回true，
	 *          如果是重定向仓位，则返回false，同时修改目标仓位为过滤器设定的值
	 * @param stdCode 标准化合约代码
	 * @param targetPos 目标仓位，传入参数，会被修改
	 * @return 是否过滤掉了，如果过滤掉了，该持仓就不加入最终组合目标仓位
	 */
	bool		is_filtered_by_code(const char* stdCode, double& targetPos);

	/**
	 * @brief 检查是否被执行器过滤器过滤掉了
	 * @details 检查指定的执行器ID是否被过滤器过滤掉了
	 * @param execid 执行器ID
	 * @return 是否被过滤掉了，如果过滤掉了，该执行器就不执行任何信号了
	 */
	bool		is_filtered_by_executer(const char* execid);

private:
	//////////////////////////////////////////////////////////////////////////
	/**
	 * @brief 过滤器动作类型
	 * @details 定义过滤器可以执行的动作类型
	 */
	typedef enum tagFilterAction
	{
		FA_Ignore,		///< 忽略，即维持原有仓位
		FA_Redirect,	///< 重定向持仓，即同步到指定目标仓位
		FA_None = 99	///< 无动作
	} FilterAction;

	/**
	 * @brief 过滤器项结构
	 * @details 定义单个过滤器项的结构，包含关键字、动作和目标仓位
	 */
	typedef struct _FilterItem
	{
		std::string		_key;		///< 关键字，用于匹配策略名称或合约代码
		FilterAction	_action;	///< 过滤操作，忽略或重定向
		double			_target;	///< 目标仓位，只有当_action为FA_Redirect才生效
	} FilterItem;

	/**
	 * @brief 过滤器映射类型
	 * @details 定义了从字符串到过滤器项的映射
	 */
	typedef wt_hashmap<std::string, FilterItem>	FilterMap;
	FilterMap		_stra_filters;	///< 策略过滤器映射，键为策略名称

	FilterMap		_code_filters;	///< 代码过滤器映射，包括合约代码和品种代码，同一时间只有一个生效，合约代码优先级高于品种代码

	/**
	 * @brief 执行器过滤器映射类型
	 * @details 定义了从执行器ID到是否过滤的映射
	 */
	typedef wt_hashmap<std::string, bool>	ExecuterFilters;
	ExecuterFilters	_exec_filters;	///< 执行器过滤器映射，键为执行器ID，值为是否过滤

	std::string		_filter_file;		///< 过滤器配置文件路径
	uint64_t		_filter_timestamp;	///< 过滤器文件时间戳，用于检测文件是否变化
	EventNotifier*	_notifier;			///< 事件通知器指针，用于发送过滤器相关事件
};

NS_WTP_END
