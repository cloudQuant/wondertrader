/**
 * @file WtExecuterFactory.cpp
 * @brief 执行器工厂实现文件
 * @details 实现了执行器工厂类，用于加载和管理执行器工厂，创建各种类型的执行单元
 */
#include "WtExecuterFactory.h"

#include "../Share/StdUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../WTSTools/WTSLogger.h"

#include <boost/filesystem.hpp>


USING_NS_WTP;

//////////////////////////////////////////////////////////////////////////
// WtExecuterFactory实现

/**
 * @brief 加载执行器工厂
 * @param path 执行器工厂动态库所在目录路径
 * @return 是否加载成功
 */
bool WtExecuterFactory::loadFactories(const char* path)
{
	if (!StdFile::exists(path))
	{
		WTSLogger::error("Directory {} of executer factory not exists", path);
		return false;
	}

	boost::filesystem::path myPath(path);
	boost::filesystem::directory_iterator endIter;
	for (boost::filesystem::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		if (boost::filesystem::is_directory(iter->path()))
			continue;

#ifdef _WIN32
		if (iter->path().extension() != ".dll")
			continue;
#else //_UNIX
		if (iter->path().extension() != ".so")
			continue;
#endif

		const std::string& path = iter->path().string();

		DllHandle hInst = DLLHelper::load_library(path.c_str());
		if (hInst == NULL)
		{
			continue;
		}

		FuncCreateExeFact creator = (FuncCreateExeFact)DLLHelper::get_symbol(hInst, "createExecFact");
		if (creator == NULL)
		{
			DLLHelper::free_library(hInst);
			continue;
		}

		ExeFactInfo fInfo;
		fInfo._module_inst = hInst;
		fInfo._module_path = iter->path().string();
		fInfo._creator = creator;
		fInfo._remover = (FuncDeleteExeFact)DLLHelper::get_symbol(hInst, "deleteExecFact");
		fInfo._fact = fInfo._creator();

		_factories[fInfo._fact->getName()] = fInfo;

		WTSLogger::info("Executer factory {} loaded", fInfo._fact->getName());
	}

	return true;
}

/**
 * @brief 创建普通执行单元
 * @param factname 工厂名称
 * @param unitname 单元名称
 * @return 执行单元智能指针
 */
ExecuteUnitPtr WtExecuterFactory::createExeUnit(const char* factname, const char* unitname)
{
	// 查找指定名称的工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return ExecuteUnitPtr();

	// 获取工厂信息并创建执行单元
	ExeFactInfo& fInfo = (ExeFactInfo&)it->second;
	ExecuteUnit* unit = fInfo._fact->createExeUnit(unitname);
	if (unit == NULL)
	{
		WTSLogger::error("Createing execution unit failed: {}.{}", factname, unitname);
		return ExecuteUnitPtr();
	}
	// 将执行单元封装到ExeUnitWrapper中
	return ExecuteUnitPtr(new ExeUnitWrapper(unit, fInfo._fact));
}

/**
 * @brief 创建差价执行单元
 * @param factname 工厂名称
 * @param unitname 单元名称
 * @return 执行单元智能指针
 */
ExecuteUnitPtr WtExecuterFactory::createDiffExeUnit(const char* factname, const char* unitname)
{
	// 查找指定名称的工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return ExecuteUnitPtr();

	// 获取工厂信息并创建差价执行单元
	ExeFactInfo& fInfo = (ExeFactInfo&)it->second;
	ExecuteUnit* unit = fInfo._fact->createDiffExeUnit(unitname);
	if (unit == NULL)
	{
		WTSLogger::error("Createing diff execution unit failed: {}.{}", factname, unitname);
		return ExecuteUnitPtr();
	}
	// 将执行单元封装到ExeUnitWrapper中
	return ExecuteUnitPtr(new ExeUnitWrapper(unit, fInfo._fact));
}

/**
 * @brief 创建套利执行单元
 * @param factname 工厂名称
 * @param unitname 单元名称
 * @return 执行单元智能指针
 */
ExecuteUnitPtr WtExecuterFactory::createArbiExeUnit(const char* factname, const char* unitname)
{
	// 查找指定名称的工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return ExecuteUnitPtr();

	// 获取工厂信息并创建套利执行单元
	ExeFactInfo& fInfo = (ExeFactInfo&)it->second;
	ExecuteUnit* unit = fInfo._fact->createArbiExeUnit(unitname);
	if (unit == NULL)
	{
		WTSLogger::error("Createing arbi execution unit failed: {}.{}", factname, unitname);
		return ExecuteUnitPtr();
	}
	// 将执行单元封装到ExeUnitWrapper中
	return ExecuteUnitPtr(new ExeUnitWrapper(unit, fInfo._fact));
}

/**
 * @brief 创建普通执行单元
 * @param name 执行单元名称，格式为“工厂名.单元名”
 * @return 执行单元智能指针
 */
ExecuteUnitPtr WtExecuterFactory::createExeUnit(const char* name)
{
	// 将名称按点分隔符分割为工厂名和单元名
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return ExecuteUnitPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	// 查找指定名称的工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return ExecuteUnitPtr();

	// 获取工厂信息并创建执行单元
	ExeFactInfo& fInfo = (ExeFactInfo&)it->second;
	ExecuteUnit* unit = fInfo._fact->createExeUnit(unitname);
	if (unit == NULL)
	{
		WTSLogger::error("Createing execution unit failed: {}", name);
		return ExecuteUnitPtr();
	}
	// 将执行单元封装到ExeUnitWrapper中
	return ExecuteUnitPtr(new ExeUnitWrapper(unit, fInfo._fact));
}

/**
 * @brief 创建差价执行单元
 * @param name 执行单元名称，格式为“工厂名.单元名”
 * @return 执行单元智能指针
 */
ExecuteUnitPtr WtExecuterFactory::createDiffExeUnit(const char* name)
{
	// 将名称按点分隔符分割为工厂名和单元名
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return ExecuteUnitPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	// 查找指定名称的工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return ExecuteUnitPtr();

	// 获取工厂信息并创建差价执行单元
	ExeFactInfo& fInfo = (ExeFactInfo&)it->second;
	ExecuteUnit* unit = fInfo._fact->createDiffExeUnit(unitname);
	if (unit == NULL)
	{
		WTSLogger::error("Createing execution unit failed: {}", name);
		return ExecuteUnitPtr();
	}
	// 将执行单元封装到ExeUnitWrapper中
	return ExecuteUnitPtr(new ExeUnitWrapper(unit, fInfo._fact));
}

/**
 * @brief 创建套利执行单元
 * @param name 执行单元名称，格式为“工厂名.单元名”
 * @return 执行单元智能指针
 */
ExecuteUnitPtr WtExecuterFactory::createArbiExeUnit(const char* name)
{
	// 将名称按点分隔符分割为工厂名和单元名
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return ExecuteUnitPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	// 查找指定名称的工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return ExecuteUnitPtr();

	// 获取工厂信息并创建套利执行单元
	ExeFactInfo& fInfo = (ExeFactInfo&)it->second;
	ExecuteUnit* unit = fInfo._fact->createArbiExeUnit(unitname);
	if (unit == NULL)
	{
		WTSLogger::error("Createing execution unit failed: {}", name);
		return ExecuteUnitPtr();
	}
	// 将执行单元封装到ExeUnitWrapper中
	return ExecuteUnitPtr(new ExeUnitWrapper(unit, fInfo._fact));
}
