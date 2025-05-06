/**
 * @file IExecCommand.h
 * @brief 执行命令接口定义
 * @details 定义了执行器存根接口(IExecuterStub)和执行命令接口(IExecCommand)，
 *          用于实现交易执行器和交易引擎之间的交互
 */
#pragma once
#include "../Includes/FasterDefs.h"
#include <stdint.h>

NS_WTP_BEGIN
class WTSCommodityInfo;
class WTSSessionInfo;
class IHotMgr;
class WTSTickData;

/**
 * @brief 执行器存根接口
 * @details 由交易引擎实现，提供给执行器调用交易引擎功能的接口
 */
class IExecuterStub
{
public:
	/**
	 * @brief 获取当前时间
	 * @return 当前时间戳
	 */
	virtual uint64_t get_real_time() = 0;

	/**
	 * @brief 获取合约信息
	 * @param stdCode 标准合约代码
	 * @return 合约信息指针
	 */
	virtual WTSCommodityInfo* get_comm_info(const char* stdCode) = 0;

	/**
	 * @brief 获取交易时段信息
	 * @param stdCode 标准合约代码
	 * @return 交易时段信息指针
	 */
	virtual WTSSessionInfo* get_sess_info(const char* stdCode) = 0;

	/**
	 * @brief 获取主力合约管理器
	 * @return 主力合约管理器指针
	 */
	virtual IHotMgr* get_hot_mon() = 0;

	/**
	 * @brief 获取当前交易日
	 * @return 交易日，格式为YYYYMMDD
	 */
	virtual uint32_t get_trading_day() = 0;
};

/**
 * @brief 执行命令接口
 * @details 交易执行器的基础接口，定义了执行器需要实现的基本功能
 */
class IExecCommand
{
public:
	/**
	 * @brief 构造函数
	 * @param name 执行器名称
	 */
	IExecCommand(const char* name) :_stub(NULL), _name(name){}

	/**
	 * @brief 设置目标仓位
	 * @param targets 目标仓位映射表，键为合约代码，值为目标仓位
	 * @details 根据输入的目标仓位映射表调整成交易代码的仓位
	 */
	virtual void set_position(const wt_hashmap<std::string, double>& targets) {}

	/**
	 * @brief 合约仓位变动通知
	 * @param stdCode 标准合约代码
	 * @param diffPos 仓位变动量，正数表示增加，负数表示减少
	 * @details 当成交后合约仓位发生变化时调用
	 */
	virtual void on_position_changed(const char* stdCode, double diffPos) {}

	/**
	 * @brief 实时行情回调
	 * @param stdCode 标准合约代码
	 * @param newTick 新的Tick数据
	 * @details 当收到合约的新Tick数据时调用
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) {}

	/**
	 * @brief 设置执行器存根
	 * @param stub 执行器存根指针
	 * @details 将交易引擎实现的存根接口设置给执行器，以便执行器调用引擎功能
	 */
	inline void setStub(IExecuterStub* stub) { _stub = stub; }

	/**
	 * @brief 获取执行器名称
	 * @return 执行器名称
	 */
	inline const char* name() const { return _name.c_str(); }

	/**
	 * @brief 设置执行器名称
	 * @param name 新的执行器名称
	 */
	inline void setName(const char* name) { _name = name; }

protected:
	IExecuterStub*	_stub;	//!< 执行器存根指针
	std::string		_name;	//!< 执行器名称
};
NS_WTP_END