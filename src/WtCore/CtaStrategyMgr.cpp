/**
 * @file CtaStrategyMgr.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief CTA策略管理器实现
 * @details 本文件实现了CTA策略管理器类，负责加载策略工厂库、
 *          创建和管理策略实例，为系统中的CTA策略执行提供支持
 */
#include "CtaStrategyMgr.h"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../WTSTools/WTSLogger.h"

#include <boost/filesystem.hpp>


/**
 * @brief 构造函数
 * @details 初始化CTA策略管理器
 */
CtaStrategyMgr::CtaStrategyMgr()
{
}


/**
 * @brief 析构函数
 * @details 清理CTA策略管理器资源
 */
CtaStrategyMgr::~CtaStrategyMgr()
{
}

/**
 * @brief 加载策略工厂
 * @details 从指定目录加载全部CTA策略工厂动态库
 *          1. 首先检查目录是否存在
 *          2. 遍历目录中的所有文件
 *          3. 筛选有效的动态库文件(.dll或.so)
 *          4. 尝试加载并获取策略工厂创建函数
 *          5. 创建工厂实例并存储相关信息
 * @param path 策略工厂动态库所在目录路径
 * @return 是否成功加载
 */
bool CtaStrategyMgr::loadFactories(const char* path)
{
	// 检查目录是否存在
	if (!StdFile::exists(path))
	{
		WTSLogger::error("Directory {} of CTA strategy factory not exists", path);
		return false;
	}

	uint32_t count = 0;
	boost::filesystem::path myPath(path);
	boost::filesystem::directory_iterator endIter;
	// 遍历目录中的所有文件
	for (boost::filesystem::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		// 跳过子目录
		if (boost::filesystem::is_directory(iter->path()))
			continue;

		// 根据不同平台筛选动态库文件
#ifdef _WIN32
		if (iter->path().extension() != ".dll")
			continue;
#else //_UNIX
		if (iter->path().extension() != ".so")
			continue;
#endif

		// 加载动态库
		DllHandle hInst = DLLHelper::load_library(iter->path().string().c_str());
		if (hInst == NULL)
			continue;

		// 获取工厂创建函数
		FuncCreateStraFact creator = (FuncCreateStraFact)DLLHelper::get_symbol(hInst, "createStrategyFact");
		if (creator == NULL)
		{
			DLLHelper::free_library(hInst);
			continue;
		}

		// 创建工厂实例
		ICtaStrategyFact* fact = creator();
		if(fact != NULL)
		{
			// 存储工厂信息
			StraFactInfo& fInfo = _factories[fact->getName()];
			fInfo._module_inst = hInst;
			fInfo._module_path = iter->path().string();
			fInfo._creator = creator;
			fInfo._remover = (FuncDeleteStraFact)DLLHelper::get_symbol(hInst, "deleteStrategyFact");
			fInfo._fact = fact;

			WTSLogger::info("CTA strategy factory[{}] loaded", fact->getName());

			count++;
		}
		else
		{
			DLLHelper::free_library(hInst);
			continue;
		}
		
	}

	WTSLogger::info("{} CTA strategy factories in directory[{}] loaded", count, path);

	return true;
}

/**
 * @brief 创建策略实例（通过工厂名和策略名）
 * @details 使用指定的工厂名和策略名创建策略实例
 *          1. 查找指定名称的工厂
 *          2. 如果找到工厂，则使用其创建策略实例
 *          3. 将创建的策略实例添加到策略映射表中
 * @param factname 工厂名称
 * @param unitname 策略名称
 * @param id 策略ID，唯一标识字符串
 * @return 策略实例智能指针，如果创建失败则返回空指针
 */
CtaStrategyPtr CtaStrategyMgr::createStrategy(const char* factname, const char* unitname, const char* id)
{
	// 查找工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return CtaStrategyPtr(); // 如果工厂不存在，返回空指针

	// 使用工厂创建策略实例
	StraFactInfo& fInfo = (StraFactInfo&)it->second;
	CtaStrategyPtr ret(new CtaStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact));
	// 将策略实例添加到策略映射表
	_strategies[id] = ret;
	return ret;
}

/**
 * @brief 创建策略实例（通过完整策略名称）
 * @details 使用完整策略名称创建策略实例，完整名称格式为“工厂名.策略名”
 *          1. 解析完整策略名称，提取工厂名和策略名
 *          2. 查找指定名称的工厂
 *          3. 如果找到工厂，则使用其创建策略实例
 *          4. 将创建的策略实例添加到策略映射表中
 * @param name 完整策略名称，格式如“MyFactory.MyStrategy”
 * @param id 策略ID，唯一标识字符串
 * @return 策略实例智能指针，如果创建失败则返回空指针
 */
CtaStrategyPtr CtaStrategyMgr::createStrategy(const char* name, const char* id)
{
	// 解析完整策略名称，按“.”分隔
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2) // 检查是否有效名称
		return CtaStrategyPtr();

	// 提取工厂名和策略名
	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	// 查找工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return CtaStrategyPtr(); // 如果工厂不存在，返回空指针

	// 使用工厂创建策略实例
	StraFactInfo& fInfo = (StraFactInfo&)it->second;
	CtaStrategyPtr ret(new CtaStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact));
	// 将策略实例添加到策略映射表
	_strategies[id] = ret;
	return ret;
}

/**
 * @brief 获取策略实例
 * @details 根据ID获取已创建的策略实例
 * @param id 策略ID
 * @return 策略实例智能指针，如果不存在则返回空指针
 */
CtaStrategyPtr CtaStrategyMgr::getStrategy(const char* id)
{
	// 在策略映射表中查找指定ID的策略
	auto it = _strategies.find(id);
	if (it == _strategies.end())
		return CtaStrategyPtr(); // 如果策略不存在，返回空指针

	return it->second;
}

