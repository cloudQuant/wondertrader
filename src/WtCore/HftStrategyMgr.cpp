/**
 * @file HftStrategyMgr.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief 高频交易策略管理器实现
 * @details 实现高频交易策略的工厂加载、创建和管理功能
 */
#include "HftStrategyMgr.h"

#include <boost/filesystem.hpp>

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../WTSTools/WTSLogger.h"


/**
 * @brief 构造函数
 * @details 初始化高频交易策略管理器
 */
HftStrategyMgr::HftStrategyMgr()
{
	// 无需进行额外初始化
}


/**
 * @brief 析构函数
 * @details 清理高频交易策略管理器资源，策略工厂和策略实例会通过智能指针自动清理
 */
HftStrategyMgr::~HftStrategyMgr()
{
	// 工厂和策略实例会通过智能指针自动清理
}

/**
 * @brief 加载策略工厂
 * @param path 策略工厂动态库文件路径
 * @return 是否成功加载
 * @details 从指定路径加载所有策略工厂动态库，包括 .dll 或 .so 文件
 */
bool HftStrategyMgr::loadFactories(const char* path)
{
	// 检查路径是否存在
	if (!StdFile::exists(path))
	{
		WTSLogger::error("Directory {} of HFT strategy factory not exists", path);
		return false;
	}

	uint32_t count = 0;
	boost::filesystem::path myPath(path);
	boost::filesystem::directory_iterator endIter;
	// 遍历目录中的文件
	for (boost::filesystem::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		// 跳过子目录
		if (boost::filesystem::is_directory(iter->path()))
			continue;

		// 根据不同操作系统加载不同扩展名的动态库
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

		// 获取创建策略工厂的函数
		FuncCreateHftStraFact creator = (FuncCreateHftStraFact)DLLHelper::get_symbol(hInst, "createStrategyFact");
		if (creator == NULL)
		{
			// 如果没有找到创建函数，释放动态库
			DLLHelper::free_library(hInst);
			continue;
		}
		
		// 创建策略工厂实例
		IHftStrategyFact* pFact = creator();
		if (pFact != NULL)
		{
			// 创建成功，将工厂信息保存到映射表中
			StraFactInfo& fInfo = _factories[pFact->getName()];

			fInfo._module_inst = hInst;
			fInfo._module_path = iter->path().string();
			fInfo._creator = creator;
			fInfo._remover = (FuncDeleteHftStraFact)DLLHelper::get_symbol(hInst, "deleteStrategyFact");
			fInfo._fact = pFact;
			WTSLogger::info("HFT strategy factory[{}] loaded", pFact->getName());

			count++;
		}
		else
		{
			// 创建失败，释放动态库
			DLLHelper::free_library(hInst);
			continue;
		}
	}

	// 记录加载的策略工厂数量
	WTSLogger::info("{} HFT strategy factories in directory[{}] loaded", count, path);

	return true;
}

/**
 * @brief 创建策略（重载版）
 * @param factname 工厂名称
 * @param unitname 策略名称
 * @param id 策略ID，唯一标识
 * @return 策略智能指针，如果工厂不存在则返回空指针
 * @details 直接指定工厂名称和策略名称，创建并管理策略实例
 */
HftStrategyPtr HftStrategyMgr::createStrategy(const char* factname, const char* unitname, const char* id)
{
	// 在已加载的工厂中查找指定名称的工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return HftStrategyPtr();

	// 获取工厂信息，创建策略实例
	StraFactInfo& fInfo = (StraFactInfo&)it->second;
	// 使用工厂创建策略实例，并用HftStraWrapper封装
	HftStrategyPtr ret(new HftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact));
	// 将策略添加到策略映射表中
	_strategies[id] = ret;
	return ret;
}

/**
 * @brief 创建策略
 * @param name 策略名称，格式为“工厂.策略”如"XTCtaFact.DualThrust"
 * @param id 策略ID，唯一标识
 * @return 策略智能指针，如果格式错误或工厂不存在则返回空指针
 * @details 根据策略全名（工厂.策略格式）创建策略实例
 */
HftStrategyPtr HftStrategyMgr::createStrategy(const char* name, const char* id)
{
	// 分解全名，格式为工厂名.策略名
	StringVector ay = StrUtil::split(name, ".");
	// 如果格式不正确，返回空指针
	if (ay.size() < 2)
		return HftStrategyPtr();

	// 提取工厂名称和策略名称
	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	// 在已加载的工厂中查找指定名称的工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return HftStrategyPtr();

	// 获取工厂信息，创建策略实例
	StraFactInfo& fInfo = (StraFactInfo&)it->second;
	// 使用工厂创建策略实例，并用HftStraWrapper封装
	HftStrategyPtr ret(new HftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact));
	// 将策略添加到策略映射表中
	_strategies[id] = ret;
	return ret;
}

/**
 * @brief 获取策略
 * @param id 策略ID
 * @return 策略智能指针，如果策略不存在则返回空指针
 * @details 根据ID获取已创建的策略实例
 */
HftStrategyPtr HftStrategyMgr::getStrategy(const char* id)
{
	// 在策略映射表中查找指定 ID 的策略
	auto it = _strategies.find(id);
	if (it == _strategies.end())
		return HftStrategyPtr();

	return it->second;
}