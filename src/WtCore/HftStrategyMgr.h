/**
 * @file HftStrategyMgr.h
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief 高频交易策略管理器头文件
 * @details 定义了高频交易策略的包装器和管理器，用于加载、创建和管理高频交易策略
 */
#pragma once
#include <memory>
#include <map>
#include <mutex>
#include <unordered_set>
#include <boost/core/noncopyable.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/HftStrategyDefs.h"
#include "../Includes/IStrategyPluginV2.h"

#include "../Share/DLLHelper.hpp"

/**
 * @brief 高频交易策略包装器
 * @details 封装了策略实例和对应的工厂接口，提供生命周期管理功能
 */
class HftStraWrapper
{
public:
	/**
	 * @brief 构造函数
	 * @param stra 策略实例指针
	 * @param fact 策略工厂接口指针
	 */
	HftStraWrapper(HftStrategy* stra, IHftStrategyFact* fact) :_stra(stra), _fact(fact){}

	/**
	 * @brief 析构函数
	 * @details 负责释放策略实例资源
	 */
	~HftStraWrapper()
	{
		if (_stra)
		{
			_fact->deleteStrategy(_stra);
			_stra = nullptr;
		}
	}

	/**
	 * @brief 获取策略实例
	 * @return 策略实例指针
	 */
	HftStrategy* self(){ return _stra; }

private:
	HftStrategy*		_stra;	//!< 策略实例指针
	IHftStrategyFact*	_fact;	//!< 策略工厂接口指针
};

/**
 * @brief 高频交易策略智能指针类型
 * @details 使用std::shared_ptr包装HftStraWrapper，提供自动内存管理
 */
typedef std::shared_ptr<HftStraWrapper>	HftStrategyPtr;

/**
 * @brief 高频交易策略管理器
 * @details 负责策略工厂的加载及策略实例的创建和管理，继承自 boost::noncopyable 确保不可复制
 */
class HftStrategyMgr : private boost::noncopyable
{
public:
	/**
	 * @brief 构造函数
	 */
	HftStrategyMgr();

	/**
	 * @brief 析构函数
	 */
	~HftStrategyMgr();

public:
	/**
	 * @brief 加载策略工厂
	 * @param path 策略工厂动态库文件路径
	 * @return 是否成功加载
	 * @details 从指定路径加载所有策略工厂动态库
	 */
	bool loadFactories(const char* path);

	/**
	 * @brief 创建策略
	 * @param name 策略名称，格式为“工厂.策略”如"XTCtaFact.DualThrust"
	 * @param id 策略ID，唯一标识
	 * @return 策略智能指针
	 * @details 创建并返回策略实例，会将实例添加到内部策略映射表中
	 */
	HftStrategyPtr createStrategy(const char* name, const char* id);

	/**
	 * @brief 创建策略（重载版）
	 * @param factname 工厂名称
	 * @param unitname 策略名称
	 * @param id 策略ID，唯一标识
	 * @return 策略智能指针
	 * @details 与上一个函数的区别在于工厂名称和策略名称是分开的
	 */
	HftStrategyPtr createStrategy(const char* factname, const char* unitname, const char* id);

	/**
	 * @brief 获取策略
	 * @param id 策略ID
	 * @return 策略智能指针
	 * @details 根据ID获取已创建的策略实例
	 */
	HftStrategyPtr getStrategy(const char* id);

private:
	// Manager-created wrappers share this owner through their shared_ptr
	// deleter; plugin ABI and HftStraWrapper's public layout stay unchanged.
	struct FactoryOwner
	{
		FactoryOwner(IHftStrategyFact* fact, const WtStrategyPluginApiV2* api, DllHandle module)
			: _fact(fact), _api(api), _module_inst(module) {}
		FactoryOwner(const FactoryOwner&) = delete;
		FactoryOwner& operator=(const FactoryOwner&) = delete;
		~FactoryOwner()
		{
			// Card 9a: only a plugin promising synchronous teardown is asked to
			// destroy its factory; otherwise the factory stays quarantined.
			if (_active && _fact && wt_stra_plugin::hasSyncTeardown(_api))
			{
				try
				{
					_api->destroy_factory(static_cast<void*>(_fact));
				}
				catch (...)
				{
					// Contract violation (no exception may cross the C table);
					// nothing else references the factory, keep the module mapped.
				}
			}
		}

		IHftStrategyFact* _fact;
		const WtStrategyPluginApiV2* _api;
		DllHandle _module_inst; // the loader reference remains resident
		bool _active = false;  // failed/unpublished attempts stay quarantined
	};

	/**
	 * @brief 策略工厂信息结构体
	 * @details 存储策略工厂动态库的路径、句柄和接口指针等信息
	 */
	typedef struct _StraFactInfo
	{
		std::string		_module_path;	//!< 模块路径
		DllHandle		_module_inst;	//!< 动态库句柄
		IHftStrategyFact*	_fact;		//!< 策略工厂接口
		FuncCreateHftStraFact	_creator;	//!< 创建工厂函数
		FuncDeleteHftStraFact	_remover;	//!< 删除工厂函数
		std::shared_ptr<FactoryOwner> _owner; //!< manager-created wrapper lifetime

		/**
		 * @brief 构造函数
		 * @details 初始化成员变量
		 */
		_StraFactInfo(const _StraFactInfo&) = delete;
		_StraFactInfo& operator=(const _StraFactInfo&) = delete;

		_StraFactInfo()
		{
			_module_inst = NULL;
			_fact = NULL;
			_creator = NULL;
			_remover = NULL;
		}

		/**
		 * @brief 析构函数
		 * @details 释放工厂实例
		 */
		~_StraFactInfo()
		{
			// Legacy in-process registrations may populate only _fact/_remover.
			// Published modules use _owner and are deleted by its final holder.
			if (_fact && !_owner)
			{
				if (_remover)
					_remover(_fact);
				_fact = nullptr;
			}
		}
	} StraFactInfo;
	/**
	 * @brief 策略工厂映射表类型
	 * @details 使用wt_hashmap存储工厂名称与工厂信息的映射关系
	 */
	// Node-based storage keeps the owning record stable and avoids copying
	// its remover-bearing value during hash table growth.
	typedef std::map<std::string, StraFactInfo> StraFactMap;

	// The synchronization and conflict state must outlive factory destruction,
	// whose plugin remover is invoked when _factories is destroyed.
	std::mutex _factories_mutex;
	std::unordered_set<DllHandle> _conflicting_factory_modules;
	StraFactMap	_factories;	//!< 存储已加载的策略工厂

	/**
	 * @brief 策略映射表类型
	 * @details 使用wt_hashmap存储策略ID与策略实例的映射关系
	 */
	typedef wt_hashmap<std::string, HftStrategyPtr> StrategyMap;
	std::mutex _strategies_mutex;
	StrategyMap	_strategies;	//!< 存储已创建的策略实例
};

