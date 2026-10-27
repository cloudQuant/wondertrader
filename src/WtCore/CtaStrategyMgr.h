/**
 * @file CtaStrategyMgr.h
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief CTA策略管理器头文件
 * @details 本文件定义了CTA策略管理相关的类，包括策略包装器和策略管理器
 *          负责加载策略工厂模块、创建和管理策略实例
 */
#pragma once
#include <memory>
#include <boost/core/noncopyable.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/CtaStrategyDefs.h"

#include "../Share/DLLHelper.hpp"

/**
 * @brief CTA策略包装器类
 * @details 用于包装CTA策略对象，管理其生命周期，并提供访问策略实例的接口
 */
class CtaStraWrapper
{
public:
	/**
	 * @brief 构造函数
	 * @param stra 策略实例指针
	 * @param fact 策略工厂指针
	 */
	CtaStraWrapper(CtaStrategy* stra, ICtaStrategyFact* fact) :_stra(stra), _fact(fact){}

	/**
	 * @brief 析构函数
	 * @details 释放策略实例资源，通过对应工厂删除策略实例
	 */
	~CtaStraWrapper()
	{
		if (_stra)
		{
			_fact->deleteStrategy(_stra);
		}
	}

	/**
	 * @brief 获取策略实例指针
	 * @return 策略实例指针
	 */
	CtaStrategy* self(){ return _stra; }


private:
	CtaStrategy*		_stra;		//!< 策略实例指针
	ICtaStrategyFact*	_fact;		//!< 策略工厂指针
};
/**
 * @brief CTA策略智能指针类型
 * @details 使用智能指针管理CtaStraWrapper对象，自动进行内存管理
 */
typedef std::shared_ptr<CtaStraWrapper>	CtaStrategyPtr;


/**
 * @brief CTA策略管理器
 * @details 管理CTA策略工厂和策略实例，负责加载策略工厂库、创建和管理策略实例
 */
class CtaStrategyMgr : private boost::noncopyable
{
public:
	/**
	 * @brief 构造函数
	 */
	CtaStrategyMgr();

	/**
	 * @brief 析构函数
	 */
	~CtaStrategyMgr();

public:
	/**
	 * @brief 加载策略工厂
	 * @param path 策略工厂动态库所在目录路径
	 * @return 是否成功加载
	 */
	bool loadFactories(const char* path);

	/**
	 * @brief 创建策略实例（通过完整策略名称）
	 * @details 完整策略名称格式为“工厂名.策略名”
	 * @param name 完整策略名称，格式如“MyFactory.MyStrategy”
	 * @param id 策略ID，唯一标识字符串
	 * @return 策略实例智能指针，如果创建失败则返回空指针
	 */
	CtaStrategyPtr createStrategy(const char* name, const char* id);

	/**
	 * @brief 创建策略实例（通过工厂名和策略名）
	 * @param factname 工厂名称
	 * @param unitname 策略名称
	 * @param id 策略ID，唯一标识字符串
	 * @return 策略实例智能指针，如果创建失败则返回空指针
	 */
	CtaStrategyPtr createStrategy(const char* factname, const char* unitname, const char* id);

	/**
	 * @brief 获取策略实例
	 * @param id 策略ID
	 * @return 策略实例智能指针，如果不存在则返回空指针
	 */
	CtaStrategyPtr getStrategy(const char* id);
private:
	/**
	 * @brief 策略工厂信息结构体
	 * @details 存储每个已加载的策略工厂的相关信息
	 */
	typedef struct _StraFactInfo
	{
		std::string		_module_path;		//!< 模块文件路径
		DllHandle		_module_inst;		//!< 动态库实例句柄
		ICtaStrategyFact*	_fact;			//!< 策略工厂接口指针
		FuncCreateStraFact	_creator;		//!< 创建工厂函数指针
		FuncDeleteStraFact	_remover;		//!< 删除工厂函数指针
	} StraFactInfo;

	/** 
	 * @brief 策略工厂映射表类型
	 * @details 存储工厂名称到工厂信息的映射关系
	 */
	typedef wt_hashmap<std::string, StraFactInfo> StraFactMap;

	StraFactMap	_factories;		//!< 已加载的策略工厂映射表

	/**
	 * @brief 策略映射表类型
	 * @details 存储策略ID到策略实例的映射关系
	 */
	typedef wt_hashmap<std::string, CtaStrategyPtr> StrategyMap;

	StrategyMap	_strategies;		//!< 已创建的策略实例映射表
};

