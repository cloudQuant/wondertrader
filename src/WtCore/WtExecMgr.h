/*!
 * @file WtExecMgr.h
 * @author wondertrader
 * @date 2022/05/01
 * 
 * @brief 执行管理器定义文件
 * @details 执行管理器负责管理交易执行器，处理目标仓位变化，并将交易指令路由到相应的执行器
 */

#pragma once
#include <functional>
#include "WtLocalExecuter.h"

NS_WTP_BEGIN
class WtFilterMgr;

/**
 * @brief 执行器枚举回调函数类型
 * @details 用于遍历执行器时的回调函数类型定义
 */
typedef std::function<void(ExecCmdPtr)> EnumExecuterCb;

/**
 * @brief 执行管理器类
 * @details 负责管理交易执行器，处理目标仓位变化，并将交易指令路由到相应的执行器
 * @ingroup WtCore
 */
class WtExecuterMgr : private boost::noncopyable
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化执行管理器，将过滤器管理器设置为空
	 */
	WtExecuterMgr():_filter_mgr(NULL){}

	/**
	 * @brief 设置过滤器管理器
	 * @details 设置执行管理器使用的过滤器管理器
	 * @param mgr 过滤器管理器指针
	 */
	inline void set_filter_mgr(WtFilterMgr* mgr) { _filter_mgr = mgr; }

	/**
	 * @brief 添加执行器
	 * @details 将执行器添加到执行器集合中，以执行器名称为键
	 * @param executer 执行器指针
	 */
	inline void	add_executer(ExecCmdPtr executer)
	{
		_executers[executer->name()] = executer;
	}

	/**
	 * @brief 枚举所有执行器
	 * @details 遍历所有执行器，并对每个执行器调用回调函数
	 * @param cb 执行器枚举回调函数
	 */
	void	enum_executer(EnumExecuterCb cb);

	/**
	 * @brief 设置目标仓位
	 * @details 设置多个合约的目标仓位
	 * @param target_pos 目标仓位映射表，键为合约代码，值为目标仓位
	 */
	void	set_positions(wt_hashmap<std::string, double> target_pos);
	/**
	 * @brief 处理仓位变化
	 * @details 处理单个合约的目标仓位变化，并将变化传递给相应的执行器
	 * @param stdCode 标准化合约代码
	 * @param targetPos 目标仓位
	 * @param diffPos 仓位差值
	 * @param execid 执行器ID，默认为"ALL"，表示所有执行器
	 */
	void	handle_pos_change(const char* stdCode, double targetPos, double diffPos, const char* execid = "ALL");
	/**
	 * @brief 处理行情数据
	 * @details 将最新的行情数据传递给相应的执行器
	 * @param stdCode 标准化合约代码
	 * @param curTick 当前行情数据
	 */
	void	handle_tick(const char* stdCode, WTSTickData* curTick);

	/**
	 * @brief 加载路由规则
	 * @details 从配置中加载策略到执行器的路由规则
	 * @param config 配置对象
	 * @return 是否成功加载
	 */
	bool	load_router_rules(WTSVariant* config);

	/**
	 * @brief 获取策略的路由规则
	 * @details 根据策略ID获取对应的执行器集合
	 * @param strategyid 策略ID
	 * @return 执行器集合的常量引用，如果没有找到则返回包含"ALL"的默认集合
	 */
	inline const wt_hashset<std::string>& get_route(const char* strategyid)
	{
		static wt_hashset<std::string> ALL_EXECUTERS;
		if (ALL_EXECUTERS.empty())
			ALL_EXECUTERS.insert("ALL");

		if (_router_rules.empty())
			return ALL_EXECUTERS;

		auto it = _router_rules.find(strategyid);
		if (it == _router_rules.end())
			return ALL_EXECUTERS;

		return it->second;
	}

	/**
	 * @brief 清除缓存的目标仓位
	 * @details 清除所有缓存的目标仓位数据
	 */
	inline void	clear_cached_targets()
	{
		_all_cached_targets.clear();
	}

	/**
	 * @brief 将目标仓位加入缓存
	 * @details 将单个合约的目标仓位加入缓存，以便后续一次性提交
	 * @param stdCode 标准化合约代码
	 * @param targetPos 目标仓位
	 * @param execid 执行器ID，默认为"ALL"，表示所有执行器
	 */
	void	add_target_to_cache(const char* stdCode, double targetPos, const char* execid = "ALL");

	/**
	 * @brief 提交缓存的目标仓位
	 * @details 将缓存中的所有目标仓位一次性提交给相应的执行器
	 * @param scale 风控系数，默认为1.0，可以用于调整所有仓位的比例
	 */
	void	commit_cached_targets(double scale = 1.0);

private:
	/**
	 * @brief 执行器映射表类型定义
	 * @details 以执行器名称为键，执行器指针为值的映射表
	 */
	typedef wt_hashmap<std::string, ExecCmdPtr> ExecuterMap;
	ExecuterMap		_executers;    ///< 执行器映射表
	WtFilterMgr*	_filter_mgr;   ///< 过滤器管理器指针

	/**
	 * @brief 目标仓位映射表类型定义
	 * @details 以合约代码为键，目标仓位为值的映射表
	 */
	typedef wt_hashmap<std::string, double> TargetsMap;
	wt_hashmap<std::string, TargetsMap>	_all_cached_targets;  ///< 缓存的目标仓位，以执行器ID为键，目标仓位映射表为值

	/**
	 * @brief 执行器集合类型定义
	 * @details 存储执行器ID的集合
	 */
	typedef wt_hashset<std::string>	ExecuterSet;
	wt_hashmap<std::string, ExecuterSet>	_router_rules;  ///< 路由规则，以策略ID为键，执行器集合为值

	wt_hashset<std::string>	_routed_executers;  ///< 已路由的执行器集合
};
NS_WTP_END
