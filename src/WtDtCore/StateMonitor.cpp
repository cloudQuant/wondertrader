/*!
 * \file StateMonitor.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 交易状态监控器实现
 * \details 实现交易状态监控器，管理交易时间段状态的变化
 *          包括初始化交易时间段状态、运行监控线程、检测状态变化
 *          并根据每个时间点的不同处理相应的状态转换
 */
#include "StateMonitor.h"
#include "DataManager.h"

#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"


StateMonitor::StateMonitor()
	: _stopped(false)
	, _bd_mgr(NULL)
	, _dt_mgr(NULL)
{
}


StateMonitor::~StateMonitor()
{
}

bool StateMonitor::initialize(const char* filename, WTSBaseDataMgr* bdMgr, DataManager* dtMgr)
{
	_bd_mgr = bdMgr;
	_dt_mgr = dtMgr;

	if (!StdFile::exists(filename))
	{
		WTSLogger::error("State config file {} not exists", filename);
		return false;
	}

	WTSVariant* config = WTSCfgLoader::load_from_file(filename);
	if (config == NULL)
	{
		WTSLogger::error("Loading state config failed");
		return false;
	}

	auto keys = config->memberNames();
	for (const std::string& sid : keys)
	{
		WTSVariant* jItem = config->get(sid.c_str());

		WTSSessionInfo* ssInfo = _bd_mgr->getSession(sid.c_str());
		if (ssInfo == NULL)
		{
			WTSLogger::error("Trading session template [{}] not exists,state control rule skipped", sid);
			continue;
		}

		StatePtr stateInfo(new StateInfo);
		stateInfo->_sInfo = ssInfo;
		stateInfo->_init_time = jItem->getUInt32("inittime");	//初始化时间,初始化以后数据才开始接收
		stateInfo->_close_time = jItem->getUInt32("closetime");	//收盘时间,收盘后数据不再接收了
		stateInfo->_proc_time = jItem->getUInt32("proctime");	//盘后处理时间,主要把实时数据转到历史去

		strcpy(stateInfo->_session, sid.c_str());

		const auto& auctions = ssInfo->getAuctionSections();//这里面是偏移过的时间,要注意了!!!
		for(const auto& secInfo : auctions)
		{
			uint32_t stime = secInfo.first;
			uint32_t etime = secInfo.second;

			stime = stime / 100 * 60 + stime % 100;//先将时间转成分钟数
			etime = etime / 100 * 60 + etime % 100;

			stime = stime / 60 * 100 + stime % 60;//再将分钟数转成时间
			etime = etime / 60 * 100 + etime % 60;//先不考虑半夜12点的情况,目前看来,几乎没有
			stateInfo->_sections.emplace_back(StateInfo::Section({ stime, etime }));
		}

		const auto& sections = ssInfo->getTradingSections();//这里面是偏移过的时间,要注意了!!!
		for (const auto& secInfo : sections)
		{
			uint32_t stime = secInfo.first;
			uint32_t etime = secInfo.second;

			stime = stime / 100 * 60 + stime % 100;//先将时间转成分钟数
			etime = etime / 100 * 60 + etime % 100;

			stime--;//开始分钟数-1
			etime++;//结束分钟数+1

			stime = stime / 60 * 100 + stime % 60;//再将分钟数转成时间
			etime = etime / 60 * 100 + etime % 60;//先不考虑半夜12点的情况,目前看来,几乎没有
			stateInfo->_sections.emplace_back(StateInfo::Section({ stime, etime }));
		}

		_map[stateInfo->_session] = stateInfo;

		CodeSet* pCommSet =  _bd_mgr->getSessionComms(stateInfo->_session);
		if (pCommSet)
		{
			uint32_t curDate = TimeUtils::getCurDate();
			uint32_t curMin = TimeUtils::getCurMin() / 100;
			uint32_t offDate = ssInfo->getOffsetDate(curDate, curMin);
			uint32_t offMin = ssInfo->offsetTime(curMin, true);

			//先获取基准的交易日

			for (auto it = pCommSet->begin(); it != pCommSet->end(); it++)
			{
				const char* pid = (*it).c_str();

				 _bd_mgr->setTradingDate(pid,  _bd_mgr->getTradingDate(pid, offDate, offMin, false), false);
				uint32_t prevDate = TimeUtils::getNextDate(curDate, -1);
				if ((ssInfo->getOffsetMins() > 0 &&
					(! _bd_mgr->isTradingDate(pid, curDate) &&
					!(ssInfo->isInTradingTime(curMin) &&  _bd_mgr->isTradingDate(pid, prevDate)))) ||
					(ssInfo->getOffsetMins() <= 0 && ! _bd_mgr->isTradingDate(pid, offDate))
					)
				{
					WTSLogger::info("Instrument {} is in holiday", pid);
				}
			}
		}
	}
	return true;
}

/**
 * @brief 启动交易状态监控器
 * @details 创建并运行监控线程，定期检查各交易时间段状态并处理状态转换
 *          线程会每秒检查一次所有交易时间段的状态，根据当前时间自动处理状态转换
 *          包括从初始状态到收盘作业完成的全部状态流转过程
 */
void StateMonitor::run()
{
	if(_thrd == NULL)
	{
		// 创建监控线程
		_thrd.reset(new StdThread([this](){

			// 主循环，直到收到停止信号
			while (!_stopped)
			{
				static uint64_t lastTime = 0; // 上次检查时间戳

				// 控制检查频率为每秒一次
				while(true)
				{
					uint64_t now = TimeUtils::getLocalTimeNow(); // 获取当前时间戳
					if(now - lastTime >= 1000) // 时间间隔达到1秒
						break;

					if(_stopped) // 如果收到停止信号，立即退出
						break;

					// 短暂休眠，避免占用过多CPU资源
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}

				// 检查是否需要退出线程
				if(_stopped)
					break;

				// 获取当前日期和分钟数
				uint32_t curDate = TimeUtils::getCurDate(); // 当前日期，格式YYYYMMDD
				uint32_t curMin = TimeUtils::getCurMin() / 100; // 当前分钟数，格式HHMM

				// 遍历所有交易时间段，检查和更新状态
				auto it = _map.begin();
				for (; it != _map.end(); it++)
				{
					StatePtr& stateInfo = (StatePtr&)it->second; // 获取当前交易时间段的状态信息

					WTSSessionInfo* sInfo = stateInfo->_sInfo; // 获取交易时间段信息

					// 获取偏移后的日期和前一天日期，用于识别夜盘和节假日情况
					uint32_t offDate = sInfo->getOffsetDate(curDate, curMin); // 计算偏移后的日期
					uint32_t prevDate = TimeUtils::getNextDate(curDate, -1); // 前一天日期

					// 根据当前状态执行相应的状态转换逻辑
					// 各个状态有不同的转换条件和目标状态
					switch(stateInfo->_state)
					{
					// 原始状态，初始化后的默认状态
					case SS_ORIGINAL:
						{
							// 计算各个关键时间点，用于状态判断
							uint32_t offTime = sInfo->offsetTime(curMin, true);        // 当前时间的偏移值，格式HHMM
							uint32_t offInitTime = sInfo->offsetTime(stateInfo->_init_time, true);    // 初始化时间的偏移值
							uint32_t offCloseTime = sInfo->offsetTime(stateInfo->_close_time, false);  // 收盘时间的偏移值
							uint32_t aucStartTime = sInfo->getAuctionStartTime(true);                 // 集合竹开始时间

							// 检查是否所有合约都处于节假日状态
							bool isAllHoliday = true;
							std::stringstream ss_a, ss_b;  // 用于收集节假日和非节假日合约ID
							// 获取当前交易时间段关联的所有合约
							CodeSet* pCommSet =  _bd_mgr->getSessionComms(stateInfo->_session);
							// 如果存在合约集合，检查每个合约是否处于节假日
							if (pCommSet)
							{
								// 遍历交易时间段关联的所有合约
								for (auto it = pCommSet->begin(); it != pCommSet->end(); it++)
								{
									const char* pid = (*it).c_str(); // 合约ID
									/*
									 * 节假日判断逻辑：
									 * 1. 如果时间往后偏移（夜盘情况）：
									 *    a. 当前日期不是交易日，且
									 *    b. 非夜盘后半夜情况（即不满足：当前在交易时间且昨天是交易日）
									 * 2. 如果时间没有往后偏移（白盘情况）：
									 *    状态取决于算出的偏移日期是否为交易日
									 */
									if ((sInfo->getOffsetMins() > 0 &&
										(! _bd_mgr->isTradingDate(pid, curDate) &&	// 当前日期不是交易日
										!(sInfo->isInTradingTime(curMin) &&  _bd_mgr->isTradingDate(pid, prevDate)))) ||	// 当前不在交易时间或昨天不是交易日
										(sInfo->getOffsetMins() <= 0 && ! _bd_mgr->isTradingDate(pid, offDate)) // 白盘情况下偏移日期不是交易日
										)
									{
										// 当前合约处于节假日
										ss_a << pid << ","; // 收集节假日合约ID
										WTSLogger::info("Instrument {} is in holiday", pid);
									}
									else
									{
										// 当前合约不处于节假日
										ss_b << pid << ","; // 收集非节假日合约ID
										isAllHoliday = false; // 至少有1个合约不在节假日
									}
								}

							}
							else
							{
								// 如果没有合约与当前交易时间段关联，等同于节假日
								WTSLogger::info("No corresponding instrument of trading session {}[{}], changed into holiday state", sInfo->name(), stateInfo->_session);
								stateInfo->_state = SS_Holiday;
							}

							// 状态转换逻辑：根据合约状态和时间点确定下一个状态
							if(isAllHoliday) // 全部合约都处于节假日状态
							{
								WTSLogger::info("All instruments of trading session {}[{}] are in holiday, changed into holiday state", sInfo->name(), stateInfo->_session);
								stateInfo->_state = SS_Holiday; // 进入节假日状态
							}
							else if (offTime >= offCloseTime) // 当前时间已超过收盘时间
							{
								stateInfo->_state = SS_CLOSED; // 进入已收盘状态
								WTSLogger::info("Trading session {}[{}] stopped receiving data", sInfo->name(), stateInfo->_session);
							}
							else if (aucStartTime != -1 && offTime >= aucStartTime) // 集合竹已开始
							{
								if (stateInfo->isInSections(offTime)) // 如果当前时间在交易时间区间内
								{
									//if(sInfo->_schedule)
									//{
									//	_dt_mgr->preloadRtCaches();
									//}
									stateInfo->_state = SS_RECEIVING; // 进入接收数据状态
									WTSLogger::info("Trading session {}[{}] started receiving data", sInfo->name(), stateInfo->_session);
								}
								else // 如果当前时间不在交易时间区间内
								{
									// 小于市场收盘时间且不在交易时间区间内，则为中途休盘时间
									if(offTime < sInfo->getCloseTime(true))
									{
										stateInfo->_state = SS_PAUSED; // 进入休息中状态
										WTSLogger::info("Trading session {}[{}] paused receiving data", sInfo->name(), stateInfo->_session);
									}
									else
									{// 大于市场收盘时间但小于收盘设定时间，继续接收数据(主要是接收结算价)
										stateInfo->_state = SS_RECEIVING; // 进入接收数据状态
										WTSLogger::info("Trading session {}[{}] started receiving data", sInfo->name(), stateInfo->_session);
									}
									
								}
							}								
							else if (offTime >= offInitTime) // 当前时间大于等于初始化时间
							{
								stateInfo->_state = SS_INITIALIZED; // 进入已初始化状态
								WTSLogger::info("Trading session {}[{}] initialized", sInfo->name(), stateInfo->_session);
							}

							
						}
						break;
					// 已初始化状态，系统数据准备就绪后的状态
					case SS_INITIALIZED:
						{
							// 获取当前偏移时间和集合竹开始时间
							uint32_t offTime = sInfo->offsetTime(curMin, true);       // 当前时间的偏移值
							uint32_t offAucSTime = sInfo->getAuctionStartTime(true); // 集合竹开始时间
							
							// 如果没有集合竹或已经到达集合竹开始时间
							if (offAucSTime == -1 || offTime >= sInfo->getAuctionStartTime(true))
							{
								// 如果当前时间不在交易时间区间内且小于市场收盘时间，则为中途休盘状态
								if (!stateInfo->isInSections(offTime) && offTime < sInfo->getCloseTime(true))
								{
									//if (sInfo->_schedule)
									//{
									//	_dt_mgr->preloadRtCaches();
									//}
									stateInfo->_state = SS_PAUSED; // 进入休息中状态

									WTSLogger::info("Trading session {}[{}] paused receiving data", sInfo->name(), stateInfo->_session);
								}
								else
								{
									// 如果当前时间在交易时间区间内或已超过市场收盘时间
									//if (sInfo->_schedule)
									//{
									//	_dt_mgr->preloadRtCaches();
									//}
									stateInfo->_state = SS_RECEIVING; // 进入接收数据状态
									WTSLogger::info("Trading session {}[{}] started receiving data", sInfo->name(), stateInfo->_session);
								}
								
							}
						}
						break;
					// 交易中状态，正在接收数据
					case SS_RECEIVING:
						{
							// 获取当前偏移时间和收盘时间
							uint32_t offTime = sInfo->offsetTime(curMin, true);        // 当前时间的偏移值
							uint32_t offCloseTime = sInfo->offsetTime(stateInfo->_close_time, false); // 收盘时间的偏移值
							
							// 当前时间已超过收盘时间，停止接收数据
							if (offTime >= offCloseTime)
							{
								stateInfo->_state = SS_CLOSED; // 进入已收盘状态

								WTSLogger::info("Trading session {}[{}] stopped receiving data", sInfo->name(), stateInfo->_session);
							}
							// 当前时间超过集合竹开始时间
							else if (offTime >= sInfo->getAuctionStartTime(true))
							{
								// 当前时间小于市场收盘时间
								if (offTime < sInfo->getCloseTime(true))
								{
									// 如果当前时间不在交易时间区间内，则为中途休盘时间
									if (!stateInfo->isInSections(offTime))
									{
										//if (sInfo->_schedule)
										//{
										//	_dt_mgr->preloadRtCaches();
										//}
										stateInfo->_state = SS_PAUSED; // 进入休息中状态

										WTSLogger::info("Trading session {}[{}] paused receiving data", sInfo->name(), stateInfo->_session);
									}
								}
								else
								{
									// 当前时间大于等于市场收盘时间但小于收盘设定时间
									// 维持当前状态不变，继续接收数据，主要是接收结算价
								}
							}
						}
						break;
					// 休息中状态，交易暂停（如中午休市）
					case SS_PAUSED:
						{
							// 休息状态只能转换为交易状态或节假日状态
							// 这里使用偏移日期进行判断，避免周末交易时间段判断错误
							uint32_t weekDay = TimeUtils::getWeekDay(); // 获取当前星期

							// 检查是否所有合约都处于节假日状态
							bool isAllHoliday = true;
							// 获取当前交易时间段关联的所有合约
							CodeSet* pCommSet =  _bd_mgr->getSessionComms(stateInfo->_session);
							if (pCommSet)
							{
								// 遍历所有合约检查是否处于节假日
								for (auto it = pCommSet->begin(); it != pCommSet->end(); it++)
								{
									const char* pid = (*it).c_str(); // 合约ID
									// 使用与原始状态相同的节假日判断逻辑
									if ((sInfo->getOffsetMins() > 0 &&
										(! _bd_mgr->isTradingDate(pid, curDate) &&  // 当前日期不是交易日
										!(sInfo->isInTradingTime(curMin) &&  _bd_mgr->isTradingDate(pid, prevDate)))) ||  // 不处于夜盘后半夜情况
										(sInfo->getOffsetMins() <= 0 && ! _bd_mgr->isTradingDate(pid, offDate))  // 白盘情况下偏移日期不是交易日
										)
									{
										// 当前合约处于节假日
										WTSLogger::info("Instrument {} is in holiday", pid);
									}
									else
									{
										// 当前合约不处于节假日
										isAllHoliday = false; // 至少有1个合约不在节假日
									}
								}
							}
							
							// 如果不是所有合约都处于节假日状态
							if (!isAllHoliday)
							{
								uint32_t offTime = sInfo->offsetTime(curMin, true); // 获取当前偏移时间
								// 检查当前时间是否在交易时间区间内
								if (stateInfo->isInSections(offTime))
								{
									stateInfo->_state = SS_RECEIVING; // 重新进入交易状态
									WTSLogger::info("Trading session {}[{}] continued to receive data", sInfo->name(), stateInfo->_session);
								}
							}
							else
							{
								// 所有合约都处于节假日状态，转入节假日状态
								WTSLogger::info("All instruments of trading session {}[{}] are in holiday, changed into holiday state", sInfo->name(), stateInfo->_session);
								stateInfo->_state = SS_Holiday; // 进入节假日状态
							}
						}
						break;
					// 已收盘状态，交易结束
					case SS_CLOSED:
						{
							// 获取当前偏移时间和相关处理时间
							uint32_t offTime = sInfo->offsetTime(curMin, true);       // 当前时间的偏移值
							uint32_t offProcTime = sInfo->offsetTime(stateInfo->_proc_time, true); // 相关盘后处理时间
							
							// 如果当前时间已超过盘后处理时间，则进入盘后处理流程
							if (offTime >= offProcTime)
							{
								// 检查当前交易时间段是否已完成收盘作业
								if(!_dt_mgr->isSessionProceeded(stateInfo->_session))
								{
									stateInfo->_state = SS_PROCING; // 进入收盘作业中状态

									WTSLogger::info("Trading session {}[{}] started processing closing task", sInfo->name(), stateInfo->_session);
									// 调用数据管理器处理历史数据，将实时数据转入历史数据
									_dt_mgr->transHisData(stateInfo->_session);
								}
								else
								{
									// 如果已处理过，直接进入盘后已处理状态
									stateInfo->_state = SS_PROCED; // 进入盘后已处理状态
								}
							}
							// 当前时间在集合竹开始时间和市场收盘时间之间
							else if (offTime >= sInfo->getAuctionStartTime(true) && offTime <= sInfo->getCloseTime(true))
							{
								// 如果当前时间不在交易时间区间内，进入休息中状态
								if (!stateInfo->isInSections(offTime))
								{
									stateInfo->_state = SS_PAUSED; // 进入休息中状态

									WTSLogger::info("Trading session {}[{}] paused receiving data", sInfo->name(), stateInfo->_session);
								}
							}
						}
						break;
					// 收盘作业中状态，正在处理收盘数据
					case SS_PROCING:
						stateInfo->_state = SS_PROCED; // 直接转为盘后已处理状态，因为实际处理是异步进行的
						break;
					// 盘后已处理状态或节假日状态
					case SS_PROCED:
					case SS_Holiday:
						{
							// 获取当前偏移时间和初始化时间
							uint32_t offTime = sInfo->offsetTime(curMin, true);       // 当前时间的偏移值
							uint32_t offInitTime = sInfo->offsetTime(stateInfo->_init_time, true); // 初始化时间
							
							// 如果当前时间在凌晨和初始化时间之间，考虑重置状态
							if (offTime >= 0 && offTime < offInitTime)
							{
								// 检查是否所有合约都处于节假日状态
								bool isAllHoliday = true;
								CodeSet* pCommSet =  _bd_mgr->getSessionComms(stateInfo->_session);
								if (pCommSet)
								{
									// 遍历所有合约检查是否处于节假日
									for (auto it = pCommSet->begin(); it != pCommSet->end(); it++)
									{
										const char* pid = (*it).c_str(); // 合约ID
										// 使用与原始状态相同的节假日判断逻辑
										if ((sInfo->getOffsetMins() > 0 &&
											(! _bd_mgr->isTradingDate(pid, curDate) &&  // 当前日期不是交易日
											!(sInfo->isInTradingTime(curMin) &&  _bd_mgr->isTradingDate(pid, prevDate)))) ||  // 不处于夜盘后半夜情况
											(sInfo->getOffsetMins() <= 0 && ! _bd_mgr->isTradingDate(pid, offDate))  // 白盘情况下偏移日期不是交易日
											)
										{
											// 当前合约仍然处于节假日状态
										}
										else
										{
											// 当前合约不为节假日，可以重置状态
											isAllHoliday = false; // 至少有1个合约不在节假日
										}
									}
								}

								// 如果不是所有合约都处于节假日，重置状态准备新一天的交易
								if(!isAllHoliday)
								{
									stateInfo->_state = SS_ORIGINAL; // 重置为原始状态，开始新一天的状态转换循环
									WTSLogger::info("Trading session {}[{}] state reset", sInfo->name(), stateInfo->_session);
								}
							}
						}
						break;
					}
					
				} // switch结束

				// 更新上次检查时间
				lastTime = TimeUtils::getLocalTimeNow();

				// 如果所有非节假日的交易时间段都处于收盘作业中状态，进行缓存清理
				if (isAllInState(SS_PROCING) && !isAllInState(SS_Holiday))
				{
					// 清理缓存数据，避免内存泄漏
					_dt_mgr->transHisData("CMD_CLEAR_CACHE");
				}
			} // 主循环结束
		}));
	}
}

/**
 * @brief 停止交易状态监控器
 * @details 设置停止标志并等待监控线程结束
 *          在服务器关闭前调用，确保监控线程安全终止
 */
void StateMonitor::stop()
{
	_stopped = true;  // 设置停止标志，通知线程退出

	if (_thrd)
		_thrd->join();  // 等待线程执行完成并安全退出
}
