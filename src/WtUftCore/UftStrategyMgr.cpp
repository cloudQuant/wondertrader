/*!
 * \file UftStrategyMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT策略管理器实现
 * \details 实现了UFT策略管理器类，负责加载策略库、创建策略实例和管理策略生命周期
 */
#include "UftStrategyMgr.h"

#include <boost/filesystem.hpp>

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../WTSTools/WTSLogger.h"


/**
 * @brief UftStrategyMgr类的构造函数
 * @details 初始化策略管理器
 */
UftStrategyMgr::UftStrategyMgr()
{
}


/**
 * @brief UftStrategyMgr类的析构函数
 * @details 清理策略管理器资源，策略映射和工厂映射会自动释放
 */
UftStrategyMgr::~UftStrategyMgr()
{
}

/**
 * @brief 加载UFT策略工厂
 * @details 从指定路径加载所有UFT策略库，并提取其中的工厂对象
 * @param path 策略库所在目录路径
 * @return 加载成功返回true，失败返回false
 */
bool UftStrategyMgr::loadFactories(const char* path)
{
	// 检查路径是否存在
	if (!StdFile::exists(path))
	{
		WTSLogger::error("Directory {} of UFT strategy factory not exists", path);
		return false;
	}

	uint32_t count = 0;
	boost::filesystem::path myPath(path);
	boost::filesystem::directory_iterator endIter;
	// 遍历目录中的所有文件
	for (boost::filesystem::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		// 如果是子目录则跳过
		if (boost::filesystem::is_directory(iter->path()))
			continue;

		// 检查文件后缀名，Windows下应为.dll，Linux下应为.so
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

		// 获取创建工厂对象的函数
		FuncCreateUftStraFact creator = (FuncCreateUftStraFact)DLLHelper::get_symbol(hInst, "createStrategyFact");
		if (creator == NULL)
		{
			DLLHelper::free_library(hInst);
			continue;
		}
		
		// 创建工厂对象
		IUftStrategyFact* pFact = creator();
		if (pFact != NULL)
		{
			// 创建成功，保存工厂信息
			StraFactInfo& fInfo = _factories[pFact->getName()];

			fInfo._module_inst = hInst;
			fInfo._module_path = iter->path().string();
			fInfo._creator = creator;
			fInfo._remover = (FuncDeleteUftStraFact)DLLHelper::get_symbol(hInst, "deleteStrategyFact");
			fInfo._fact = pFact;
			WTSLogger::info("UFT strategy factory[{}] loaded", pFact->getName());

			count++;
		}
		else
		{
			// 创建失败，释放动态库
			DLLHelper::free_library(hInst);
			continue;
		}
	}

	WTSLogger::info("{} UFT strategy factories in directory[{}] loaded", count, path);

	return true;
}

/**
 * @brief 创建UFT策略对象
 * @details 根据策略工厂名称和策略名称创建策略对象
 * @param factname 策略工厂名称
 * @param unitname 策略名称
 * @param id 策略ID
 * @return 返回创建的策略智能指针，如果失败则返回空指针
 */
UftStrategyPtr UftStrategyMgr::createStrategy(const char* factname, const char* unitname, const char* id)
{
	// 查找工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return UftStrategyPtr();

	// 使用工厂创建策略对象
	StraFactInfo& fInfo = (StraFactInfo&)it->second;
	UftStrategyPtr ret(new UftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact));
	// 保存策略对象
	_strategies[id] = ret;
	return ret;
}

/**
 * @brief 创建UFT策略对象（使用“工厂.策略”格式的名称）
 * @details 根据“工厂.策略”形式的名称解析工厂和策略，然后创建策略对象
 * @param name 策略工厂和策略名称，格式为"factname.unitname"
 * @param id 策略ID
 * @return 返回创建的策略智能指针，如果失败则返回空指针
 */
UftStrategyPtr UftStrategyMgr::createStrategy(const char* name, const char* id)
{
	// 分解“工厂.策略”格式的名称
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return UftStrategyPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	// 查找工厂
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return UftStrategyPtr();

	// 使用工厂创建策略对象
	StraFactInfo& fInfo = (StraFactInfo&)it->second;
	UftStrategyPtr ret(new UftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact));
	// 保存策略对象
	_strategies[id] = ret;
	return ret;
}

/**
 * @brief 获取策略对象
 * @details 根据策略ID获取已创建的策略对象
 * @param id 策略ID
 * @return 返回策略对象的智能指针，如果不存在则返回空指针
 */
UftStrategyPtr UftStrategyMgr::getStrategy(const char* id)
{
	// 从策略映射表中查找指定的策略ID
	auto it = _strategies.find(id);
	if (it == _strategies.end())
		return UftStrategyPtr();

	return it->second;
}