/*!
 * \file UftStrategyMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT策略管理器头文件
 * \details 定义了UFT策略管理器类及相关封装，负责UFT策略的加载、创建和管理
 */
#pragma once
#include <memory>
#include <string>
#include <boost/core/noncopyable.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/UftStrategyDefs.h"
#include "../Includes/IStrategyPluginV2.h"

#include "../Share/DLLHelper.hpp"

/**
 * @brief UFT策略包装类
 * @details 封装了UFT策略对象及其工厂，管理策略对象的生命周期
 */
class UftStraWrapper
{
public:
	/**
	 * @brief 构造函数
	 * @param stra 策略对象指针
	 * @param fact 策略工厂指针
	 */
	UftStraWrapper(UftStrategy* stra, IUftStrategyFact* fact) :_stra(stra), _fact(fact){}
	
	/**
	 * @brief 析构函数
	 * @details 释放策略对象资源，通过工厂删除策略对象
	 */
	~UftStraWrapper()
	{
		if (_stra)
		{
			_fact->deleteStrategy(_stra);
		}
	}

	/**
	 * @brief 获取策略对象指针
	 * @return 返回内部策略对象指针
	 */
	UftStrategy* self(){ return _stra; }


private:
	UftStrategy*		_stra;	///< 策略对象指针
	IUftStrategyFact*	_fact;	///< 策略工厂指针
};

/**
 * @brief UFT策略包装器智能指针类型
 * @details 使用智能指针管理UFT策略包装器，自动处理内存管理
 */
typedef std::shared_ptr<UftStraWrapper>	UftStrategyPtr;

/**
 * @brief UFT策略管理器
 * @details 管理UFT策略工厂和策略对象，负责加载策略库、创建策略实例和管理策略生命周期
 */
class UftStrategyMgr : private boost::noncopyable
{
public:
	/**
	 * @brief 构造函数
	 */
	UftStrategyMgr();

	/**
	 * @brief 析构函数
	 */
	~UftStrategyMgr();

public:
	/**
	 * @brief 加载策略工厂
	 * @param path 策略库路径
	 * @return 加载成功返回true，失败返回false
	 */
	bool loadFactories(const char* path);

	/**
	 * @brief 创建策略
	 * @param name 策略名称
	 * @param id 策略ID
	 * @return 返回创建的策略包装器指针
	 */
	UftStrategyPtr createStrategy(const char* name, const char* id);

	/**
	 * @brief 创建策略(指定工厂名和策略名称)
	 * @param factname 策略工厂名称
	 * @param unitname 策略名称
	 * @param id 策略ID
	 * @return 返回创建的策略包装器指针
	 */
	UftStrategyPtr createStrategy(const char* factname, const char* unitname, const char* id);

	/**
	 * @brief 获取策略
	 * @param id 策略ID
	 * @return 返回策略包装器指针，如果不存在则返回nullptr
	 */
	UftStrategyPtr getStrategy(const char* id);

private:
	/**
	 * @brief 版本化工厂的所有者(卡4b=A / 卡9a)
	 * @details 管理器条目与其创建的每个策略包装共享本对象(经 shared_ptr 删除器)。
	 * 最后一个引用释放时，仅当插件声明同步拆除能力才调用 destroy_factory；否则工厂与
	 * 模块隔离到进程退出。模块句柄从不卸载(加载器引用常驻)。
	 */
	struct FactoryOwner
	{
		FactoryOwner(IUftStrategyFact* fact, const WtStrategyPluginApiV2* api, DllHandle module)
			: _fact(fact), _api(api), _module_inst(module) {}
		FactoryOwner(const FactoryOwner&) = delete;
		FactoryOwner& operator=(const FactoryOwner&) = delete;
		~FactoryOwner();

		IUftStrategyFact*				_fact;
		const WtStrategyPluginApiV2*	_api;
		DllHandle						_module_inst;
	};

	/**
	 * @brief 策略工厂信息结构体
	 * @details 存储策略工厂的动态库路径、句柄与版本化所有者
	 */
	typedef struct _StraFactInfo
	{
		std::string		_module_path;		///< 模块路径
		DllHandle		_module_inst;		///< 动态链接库句柄
		IUftStrategyFact*	_fact;			///< 策略工厂指针
		std::shared_ptr<FactoryOwner> _owner;	///< 工厂所有者(决定能否拆除)

		_StraFactInfo()
		{
			_module_inst = NULL;
			_fact = NULL;
		}
	} StraFactInfo;

	UftStrategyPtr wrapStrategy(StraFactInfo& fInfo, const char* unitname, const char* id);

	/**
	 * @brief 已加载但未发布工厂的模块(被拒/外族/非插件/工厂创建失败/重名)，常驻不卸载
	 */
	wt_hashset<std::string> _refused_modules;
	wt_hashset<std::string> _skipped_modules;

	/**
	 * @brief 策略工厂映射类型
	 * @details 使用哈希表存储策略工厂信息，以工厂名称为键
	 */
	typedef wt_hashmap<std::string, StraFactInfo> StraFactMap;

	StraFactMap	_factories;		///< 策略工厂映射表

	/**
	 * @brief 策略映射类型
	 * @details 使用哈希表存储策略对象，以策略ID为键
	 */
	typedef wt_hashmap<std::string, UftStrategyPtr> StrategyMap;

	StrategyMap	_strategies;		///< 策略映射表
};

