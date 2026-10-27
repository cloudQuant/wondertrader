/**
 * @file WtExecuterFactory.h
 * @brief 执行器工厂定义文件
 * @details 定义了执行器工厂类和执行单元封装类，用于创建和管理交易执行单元
 */
#pragma once
#include "IExecCommand.h"
#include "../Includes/ExecuteDefs.h"
#include "../Share/DLLHelper.hpp"

#include <boost/core/noncopyable.hpp>

NS_WTP_BEGIN

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 执行单元封装类
 * @details 因为执行单元是dll里创建的, 如果不封装的话, 直接delete可能会有问题
 * 所以要把工厂指针一起封装到这里, 直接调用工厂实例的deleteUnit方法释放执行单元
 */
class ExeUnitWrapper
{
public:
	/**
	 * @brief 构造函数
	 * @param unitPtr 执行单元指针
	 * @param fact 执行器工厂指针
	 */
	ExeUnitWrapper(ExecuteUnit* unitPtr, IExecuterFact* fact) :_unit(unitPtr), _fact(fact) {}

	/**
	 * @brief 析构函数
	 * @details 调用工厂的deleteExeUnit方法释放执行单元
	 */
	~ExeUnitWrapper()
	{
		if (_unit)
		{
			_fact->deleteExeUnit(_unit);
		}
	}

	/**
	 * @brief 获取执行单元指针
	 * @return 执行单元指针
	 */
	ExecuteUnit* self() { return _unit; }

private:
	/**
	 * @brief 执行单元指针
	 */
	ExecuteUnit*	_unit;

	/**
	 * @brief 执行器工厂指针
	 */
	IExecuterFact*	_fact;
};

/**
 * @brief 执行单元智能指针类型
 */
typedef std::shared_ptr<ExeUnitWrapper>	ExecuteUnitPtr;

/**
 * @brief 执行单元映射表类型
 * @details 哈希表，键为字符串，值为执行单元智能指针
 */
typedef wt_hashmap<std::string, ExecuteUnitPtr> ExecuteUnitMap;

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 执行器工厂类
 * @details 用于加载和管理执行器工厂，创建各种类型的执行单元
 */
class WtExecuterFactory : private boost::noncopyable
{
public:
	/**
	 * @brief 析构函数
	 */
	~WtExecuterFactory() {}

public:
	/**
	 * @brief 加载执行器工厂
	 * @param path 执行器工厂动态库所在目录路径
	 * @return 是否加载成功
	 */
	bool loadFactories(const char* path);

	/**
	 * @brief 创建普通执行单元
	 * @param name 执行单元名称，格式为“工厂名.单元名”
	 * @return 执行单元智能指针
	 */
	ExecuteUnitPtr createExeUnit(const char* name);

	/**
	 * @brief 创建差价执行单元
	 * @param name 执行单元名称，格式为“工厂名.单元名”
	 * @return 执行单元智能指针
	 */
	ExecuteUnitPtr createDiffExeUnit(const char* name);

	/**
	 * @brief 创建套利执行单元
	 * @param name 执行单元名称，格式为“工厂名.单元名”
	 * @return 执行单元智能指针
	 */
	ExecuteUnitPtr createArbiExeUnit(const char* name);

	/**
	 * @brief 创建普通执行单元
	 * @param factname 工厂名称
	 * @param unitname 单元名称
	 * @return 执行单元智能指针
	 */
	ExecuteUnitPtr createExeUnit(const char* factname, const char* unitname);

	/**
	 * @brief 创建差价执行单元
	 * @param factname 工厂名称
	 * @param unitname 单元名称
	 * @return 执行单元智能指针
	 */
	ExecuteUnitPtr createDiffExeUnit(const char* factname, const char* unitname);

	/**
	 * @brief 创建套利执行单元
	 * @param factname 工厂名称
	 * @param unitname 单元名称
	 * @return 执行单元智能指针
	 */
	ExecuteUnitPtr createArbiExeUnit(const char* factname, const char* unitname);

private:
	/**
	 * @brief 执行器工厂信息结构体
	 * @details 存储执行器工厂的路径、实例、函数指针等信息
	 */
	typedef struct _ExeFactInfo
	{
		/**
		 * @brief 模块路径
		 */
		std::string		_module_path;

		/**
		 * @brief 动态库句柄
		 */
		DllHandle		_module_inst;

		/**
		 * @brief 执行器工厂指针
		 */
		IExecuterFact*	_fact;

		/**
		 * @brief 创建执行器工厂的函数指针
		 */
		FuncCreateExeFact	_creator;

		/**
		 * @brief 删除执行器工厂的函数指针
		 */
		FuncDeleteExeFact	_remover;
	} ExeFactInfo;

	/**
	 * @brief 执行器工厂映射表类型
	 * @details 哈希表，键为工厂名称，值为工厂信息
	 */
	typedef wt_hashmap<std::string, ExeFactInfo> ExeFactMap;

	/**
	 * @brief 执行器工厂映射表
	 * @details 存储所有已加载的执行器工厂
	 */
	ExeFactMap	_factories;
};


NS_WTP_END
