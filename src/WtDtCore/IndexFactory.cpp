/*!
 * \file IndexFactory.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 指数工厂实现
 * \details 该文件实现了指数计算的工厂类IndexFactory，负责管理多个指数计算器，并协调它们处理实时行情数据
 */
#include "IndexFactory.h"
#include "DataManager.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/StrUtil.hpp"

/**
 * @brief 初始化指数工厂
 * @details 根据配置初始化指数工厂，包括创建线程池和所有指数计算器
 * @param config 配置项，包含线程池大小和指数配置
 * @param hotMgr 热点合约管理器指针
 * @param bdMgr 基础数据管理器指针
 * @param dataMgr 数据管理器指针
 * @return bool 初始化是否成功
 */
bool IndexFactory::init(WTSVariant* config, IHotMgr* hotMgr, IBaseDataMgr* bdMgr, DataManager* dataMgr)
{
	_hot_mgr = hotMgr;
	_bd_mgr = bdMgr;
	_data_mgr = dataMgr;

	uint32_t poolsize = config->getUInt32("poolsize");
	if (poolsize > 0)
	{
		_pool.reset(new boost::threadpool::pool(poolsize));
	}

	WTSVariant* cfgIdx = config->get("indice");
	if(cfgIdx == NULL || !cfgIdx->isArray())
	{
		return false;
	}

	auto cnt = cfgIdx->size();
	for(std::size_t i = 0; i < cnt; i++)
	{
		WTSVariant* cfgItem = cfgIdx->get(i);
		if(!cfgItem->getBoolean("active"))
			continue;

		IndexWorkerPtr worker(new IndexWorker(this));
		if (!worker->init(cfgItem))
			continue;

		_workers.emplace_back(worker);
	}

	return true;
}

/**
 * @brief 处理行情数据
 * @details 接收新的Tick数据，检查是否有订阅该合约，并分发给所有指数计算器进行处理
 * 支持使用线程池并行处理，提高性能
 * @param newTick 新的Tick数据指针
 */
void IndexFactory::handle_quote(WTSTickData* newTick)
{
	if (newTick == NULL)
		return;

	const char* fullCode = newTick->getContractInfo()->getFullCode();
	auto it = _subbed.find(fullCode);
	if (it == _subbed.end())
		return;

	newTick->retain();

	if(_pool)
	{	
		_pool->schedule([this, newTick, fullCode]() {

			for(IndexWorkerPtr& worker : _workers)
			{
				worker->handle_quote(newTick);
			}
			//这里加一个处理
			newTick->release();
		});
	}
	else
	{
		for (IndexWorkerPtr& worker : _workers)
		{
			worker->handle_quote(newTick);
		}
	}
}

/**
 * @brief 推送Tick数据
 * @details 将新的Tick数据推送到数据管理器进行存储，并设置处理标志为1
 * @param newTick 新的Tick数据指针
 */
void IndexFactory::push_tick(WTSTickData* newTick)
{
	_data_mgr->writeTick(newTick, 1);
}

/**
 * @brief 订阅合约的Tick数据
 * @details 将指定合约代码添加到订阅列表中，并从数据管理器中获取当前最新的Tick数据
 * @param fullCode 合约完整代码，格式为“交易所.代码”
 * @return WTSTickData* 当前合约的最新Tick数据指针，如果没有则返回NULL
 */
WTSTickData* IndexFactory::sub_ticks(const char* fullCode)
{
	_subbed.insert(fullCode);
	
	auto ay = StrUtil::split(fullCode, ".");
	return _data_mgr->getCurTick(ay[1].c_str(), ay[0].c_str());
}