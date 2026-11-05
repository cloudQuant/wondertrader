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

#include <cstring>
#include <memory>
#include <cstdio>
#include <exception>
#include <vector>

namespace
{
	thread_local StateMonitor* currentMonitorWorker = nullptr;

	// Reuse the checked session-date calculation for a calendar-day diagnostic.
	struct PreviousCalendarDay : WTSSessionInfo
	{
		PreviousCalendarDay() : WTSSessionInfo(-1440) {}
		~PreviousCalendarDay() override = default;
	};

	struct WTSVariantRelease
	{
		void operator()(WTSVariant* value) const noexcept
		{
			if (value != nullptr)
				value->release();
		}
	};

	// StateInfo stores HHMM without a date. Pad each source interval by one minute
	// while preserving the half-open [from, end) form. A wrapped interval is
	// represented as its two day-local pieces because StateInfo has no date axis.
	std::vector<StateInfo::Section> makeTradingSections(uint32_t startTime, uint32_t endTime)
	{
		const uint32_t minutesPerDay = 24 * 60;
		auto toMinutes = [](uint32_t hhmm) {
			return hhmm / 100 * 60 + hhmm % 100;
		};
		auto toHHMM = [](uint32_t minutes) {
			return minutes / 60 * 100 + minutes % 60;
		};

		uint32_t startMinutes = toMinutes(startTime);
		uint32_t endMinutes = toMinutes(endTime);
		const bool wrapsMidnight = startMinutes > endMinutes;
		std::vector<StateInfo::Section> result;
		if (startMinutes > 0)
			--startMinutes;
		if (endMinutes < minutesPerDay)
			++endMinutes;

		if (wrapsMidnight)
		{
			// Padding may close the overnight gap. In that case, the union of
			// both day-local pieces is the full half-open day, including 24:00's
			// exclusion as the right endpoint.
			if (startMinutes <= endMinutes)
			{
				result.emplace_back(StateInfo::Section({ 0, toHHMM(minutesPerDay) }));
				return result;
			}

			result.emplace_back(StateInfo::Section({ toHHMM(startMinutes), toHHMM(minutesPerDay) }));
			result.emplace_back(StateInfo::Section({ 0, toHHMM(endMinutes) }));
			return result;
		}

		result.emplace_back(StateInfo::Section({ toHHMM(startMinutes), toHHMM(endMinutes) }));
		return result;
	}
}


StateMonitor::StateMonitor()
	: _bd_mgr(NULL)
	, _dt_mgr(NULL)
, _stopped(false)
	{
}


StateMonitor::~StateMonitor()
{
}

bool StateMonitor::initialize(const char* filename, WTSBaseDataMgr* bdMgr, DataManager* dtMgr)
{
	if (filename == NULL || *filename == '\0' || bdMgr == NULL || !StdFile::exists(filename))
	{
		WTSLogger::error("State config file is unavailable");
		return false;
	}
	try
	{
		WTSVariant* config = WTSCfgLoader::load_from_file(filename);
		if (config == NULL)
		{
			WTSLogger::error("Loading state config failed");
			return false;
		}
		return initializeConfig(config, bdMgr, dtMgr);
	}
	catch (const std::exception& e)
	{
		WTSLogger::error("State monitor initialization failed: {}", e.what());
	}
	catch (...)
	{
		WTSLogger::error("State monitor initialization failed");
	}
	return false;
}

bool StateMonitor::initializeConfig(WTSVariant* config)
{
	return initializeConfig(config, _bd_mgr, _dt_mgr);
}

bool StateMonitor::initializeConfig(WTSVariant* config, WTSBaseDataMgr* bdMgr, DataManager* dtMgr)
{
	// This helper takes ownership of one config reference on every path.
	std::unique_ptr<WTSVariant, WTSVariantRelease> configOwner(config);
	if (!configOwner || !configOwner->isObject() || bdMgr == NULL)
		return false;

	uint32_t curDate = 0, clockTime = 0;
	if (!TimeUtils::tryGetDateTime(curDate, clockTime))
		return false;
	const uint32_t curMin = clockTime / 100000;
	auto mapping = bdMgr->getDateMappingV1();
	if (mapping.size != sizeof(wtp::DateMappingV1) || mapping.version != 1 ||
		mapping.context != bdMgr || !mapping.getTradingDate || !mapping.nextTradingDate)
		return false;

	StateMap staged;
	{
		std::lock_guard<std::mutex> lock(_state_mutex);
		staged = _map;
	}
	std::vector<std::pair<std::string, uint32_t>> dates;
	for (const std::string& sid : configOwner->memberNames())
	{
		WTSVariant* jItem = configOwner->get(sid.c_str());
		if (jItem == NULL || !jItem->isObject() || sid.size() >= sizeof(StateInfo::_session))
			return false;
		WTSSessionInfo* ssInfo = bdMgr->getSession(sid.c_str());
		if (ssInfo == NULL)
		{
			WTSLogger::error("Trading session template [{}] not exists,state control rule skipped", sid);
			continue;
		}

		StatePtr stateInfo(new StateInfo);
		stateInfo->_sInfo = ssInfo;
		stateInfo->_init_time = jItem->getUInt32("inittime");
		stateInfo->_close_time = jItem->getUInt32("closetime");
		stateInfo->_proc_time = jItem->getUInt32("proctime");
		std::memcpy(stateInfo->_session, sid.c_str(), sid.size() + 1);
		for (const auto& secInfo : ssInfo->getAuctionSections())
			stateInfo->_sections.emplace_back(StateInfo::Section({ secInfo.first, secInfo.second }));
		for (const auto& secInfo : ssInfo->getTradingSections())
		{
			const auto expanded = makeTradingSections(secInfo.first, secInfo.second);
			stateInfo->_sections.insert(stateInfo->_sections.end(), expanded.begin(), expanded.end());
		}
		staged[stateInfo->_session] = stateInfo;

		CodeSet* commodities = bdMgr->getSessionComms(stateInfo->_session);
		if (commodities == NULL)
			continue;
		uint32_t offDate = 0;
		if (!ssInfo->tryGetOffsetDate(curDate, curMin, offDate))
			return false;
		const uint32_t offMin = ssInfo->offsetTime(curMin, true);
		uint32_t weekday = 0;
		if (!TimeUtils::tryGetWeekDay(offDate, weekday))
			return false;

		uint32_t previousDate = 0;
		if (ssInfo->getOffsetMins() > 0)
		{
			PreviousCalendarDay previous;
			if (!previous.tryGetOffsetDate(curDate, curMin, previousDate))
				return false;
		}
		for (const std::string& pid : *commodities)
		{
			// Explicit-date getTradingDate is side-effect-free on weekdays.
			// On weekends it writes the shared calendar cache, so use the pure
			// nextTradingDate operation with the same commodity/calendar mapping.
			const auto result = (weekday == 0 || weekday == 6)
				? mapping.nextTradingDate(&mapping, pid.c_str(), offDate, 1, 0)
				: mapping.getTradingDate(&mapping, pid.c_str(), offDate, offMin, 0);
			if (result.status != wtp::DateMappingStatus::Ok || result.value == 0)
				return false;
			dates.emplace_back(pid, result.value);
			if ((ssInfo->getOffsetMins() > 0 &&
				(!bdMgr->isTradingDate(pid.c_str(), curDate) &&
				 !(ssInfo->isInTradingTime(curMin) && bdMgr->isTradingDate(pid.c_str(), previousDate)))) ||
				(ssInfo->getOffsetMins() <= 0 && !bdMgr->isTradingDate(pid.c_str(), offDate)))
				WTSLogger::info("Instrument {} is in holiday", pid);
		}
	}

	// No date or state is published before all checks finish. The existing
	// setter has no transactional/rollback contract: an exceptional commit can
	// leave some valid dates written. Concurrent initialization is unsupported.
	for (const auto& date : dates)
		bdMgr->setTradingDate(date.first.c_str(), date.second, false);
	{
		std::lock_guard<std::mutex> lock(_state_mutex);
		_map.swap(staged);
		_bd_mgr = bdMgr;
		_dt_mgr = dtMgr;
	}
	return true;
}

void StateMonitor::setState(StateInfo& state, SimpleState value)
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	state._state = value;
}

void StateMonitor::advanceOnce(uint32_t curDate, uint32_t curMin)
{
	// Hold ownership of the current entries without retaining the lock across
	// base-data or writer callbacks, which may query this monitor reentrantly.
	std::vector<StatePtr> states;
	{
		std::lock_guard<std::mutex> lock(_state_mutex);
		states.reserve(_map.size());
		for (const auto& entry : _map)
			states.push_back(entry.second);
	}
	for (const StatePtr& stateInfo : states)
	{
		WTSSessionInfo* sInfo = stateInfo->_sInfo; // 获取交易时间段信息

		// 获取偏移后的日期和前一天日期，用于识别夜盘和节假日情况
		uint32_t offDate = sInfo->getOffsetDate(curDate, curMin); // 计算偏移后的日期
		uint32_t prevDate = TimeUtils::getNextDate(curDate, -1); // 前一天日期

		// 根据当前状态执行相应的状态转换逻辑
		// 各个状态有不同的转换条件和目标状态
		SimpleState currentState;
		{
			std::lock_guard<std::mutex> lock(_state_mutex);
			currentState = stateInfo->_state;
		}
		switch(currentState)
		{
		// 原始状态，初始化后的默认状态
		case SS_ORIGINAL:
			{
				// 计算各个关键时间点，用于状态判断
				uint32_t offTime = sInfo->offsetTime(curMin, true);        // 当前时间的偏移值，格式HHMM
				uint32_t offInitTime = sInfo->offsetTime(stateInfo->_init_time, true);    // 初始化时间的偏移值
				uint32_t offCloseTime = sInfo->offsetTime(stateInfo->_close_time, false);  // 收盘时间的偏移值
				int32_t aucStartTime = sInfo->getAuctionStartTime(true);                 // 集合竹开始时间

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
					setState(*stateInfo, SS_Holiday);
				}

				// 状态转换逻辑：根据合约状态和时间点确定下一个状态
				if(isAllHoliday) // 全部合约都处于节假日状态
				{
					WTSLogger::info("All instruments of trading session {}[{}] are in holiday, changed into holiday state", sInfo->name(), stateInfo->_session);
					setState(*stateInfo, SS_Holiday); // 进入节假日状态
				}
				else if (offTime >= offCloseTime) // 当前时间已超过收盘时间
				{
					setState(*stateInfo, SS_CLOSED); // 进入已收盘状态
					WTSLogger::info("Trading session {}[{}] stopped receiving data", sInfo->name(), stateInfo->_session);
				}
				else if (aucStartTime != -1 && offTime >= (uint32_t)aucStartTime) // 集合竹已开始
				{
					if (stateInfo->isInSections(offTime)) // 如果当前时间在交易时间区间内
					{
						//if(sInfo->_schedule)
						//{
						//	_dt_mgr->preloadRtCaches();
						//}
						setState(*stateInfo, SS_RECEIVING); // 进入接收数据状态
						WTSLogger::info("Trading session {}[{}] started receiving data", sInfo->name(), stateInfo->_session);
					}
					else // 如果当前时间不在交易时间区间内
					{
						// 小于市场收盘时间且不在交易时间区间内，则为中途休盘时间
						if(offTime < sInfo->getCloseTime(true))
						{
							setState(*stateInfo, SS_PAUSED); // 进入休息中状态
							WTSLogger::info("Trading session {}[{}] paused receiving data", sInfo->name(), stateInfo->_session);
						}
						else
						{// 大于市场收盘时间但小于收盘设定时间，继续接收数据(主要是接收结算价)
							setState(*stateInfo, SS_RECEIVING); // 进入接收数据状态
							WTSLogger::info("Trading session {}[{}] started receiving data", sInfo->name(), stateInfo->_session);
						}

					}
				}
				else if (offTime >= offInitTime) // 当前时间大于等于初始化时间
				{
					setState(*stateInfo, SS_INITIALIZED); // 进入已初始化状态
					WTSLogger::info("Trading session {}[{}] initialized", sInfo->name(), stateInfo->_session);
				}


			}
			break;
		// 已初始化状态，系统数据准备就绪后的状态
		case SS_INITIALIZED:
			{
				// 获取当前偏移时间和集合竹开始时间
				uint32_t offTime = sInfo->offsetTime(curMin, true);       // 当前时间的偏移值
				int32_t offAucSTime = sInfo->getAuctionStartTime(true); // 集合竹开始时间

				// 如果没有集合竹或已经到达集合竹开始时间
				if (offAucSTime == -1 || offTime >= (uint32_t)sInfo->getAuctionStartTime(true))
				{
					// 如果当前时间不在交易时间区间内且小于市场收盘时间，则为中途休盘状态
					if (!stateInfo->isInSections(offTime) && offTime < sInfo->getCloseTime(true))
					{
						//if (sInfo->_schedule)
						//{
						//	_dt_mgr->preloadRtCaches();
						//}
						setState(*stateInfo, SS_PAUSED); // 进入休息中状态

						WTSLogger::info("Trading session {}[{}] paused receiving data", sInfo->name(), stateInfo->_session);
					}
					else
					{
						// 如果当前时间在交易时间区间内或已超过市场收盘时间
						//if (sInfo->_schedule)
						//{
						//	_dt_mgr->preloadRtCaches();
						//}
						setState(*stateInfo, SS_RECEIVING); // 进入接收数据状态
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
					setState(*stateInfo, SS_CLOSED); // 进入已收盘状态

					WTSLogger::info("Trading session {}[{}] stopped receiving data", sInfo->name(), stateInfo->_session);
				}
				// 当前时间超过集合竹开始时间
				else if (offTime >= (uint32_t)sInfo->getAuctionStartTime(true))
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
							setState(*stateInfo, SS_PAUSED); // 进入休息中状态

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
				[[maybe_unused]] uint32_t weekDay = TimeUtils::getWeekDay(); // 获取当前星期

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
						setState(*stateInfo, SS_RECEIVING); // 重新进入交易状态
						WTSLogger::info("Trading session {}[{}] continued to receive data", sInfo->name(), stateInfo->_session);
					}
				}
				else
				{
					// 所有合约都处于节假日状态，转入节假日状态
					WTSLogger::info("All instruments of trading session {}[{}] are in holiday, changed into holiday state", sInfo->name(), stateInfo->_session);
					setState(*stateInfo, SS_Holiday); // 进入节假日状态
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
						setState(*stateInfo, SS_PROCING); // 进入收盘作业中状态

						WTSLogger::info("Trading session {}[{}] started processing closing task", sInfo->name(), stateInfo->_session);
						// 调用数据管理器处理历史数据，将实时数据转入历史数据
						_dt_mgr->transHisData(stateInfo->_session);
					}
					else
					{
						// 如果已处理过，直接进入盘后已处理状态
						setState(*stateInfo, SS_PROCED); // 进入盘后已处理状态
					}
				}
				// 当前时间在集合竹开始时间和市场收盘时间之间
				else if (offTime >= (uint32_t)sInfo->getAuctionStartTime(true) && offTime <= sInfo->getCloseTime(true))
				{
					// 如果当前时间不在交易时间区间内，进入休息中状态
					if (!stateInfo->isInSections(offTime))
					{
						setState(*stateInfo, SS_PAUSED); // 进入休息中状态

						WTSLogger::info("Trading session {}[{}] paused receiving data", sInfo->name(), stateInfo->_session);
					}
				}
			}
			break;
		// 收盘作业中状态，正在处理收盘数据
		case SS_PROCING:
			setState(*stateInfo, SS_PROCED); // 直接转为盘后已处理状态，因为实际处理是异步进行的
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
						setState(*stateInfo, SS_ORIGINAL); // 重置为原始状态，开始新一天的状态转换循环
						WTSLogger::info("Trading session {}[{}] state reset", sInfo->name(), stateInfo->_session);
					}
				}
			}
			break;
		}

	} // switch结束
}

/**
 * @brief 启动交易状态监控器
 * @details 创建并运行监控线程，定期检查各交易时间段状态并处理状态转换
 *          线程会每秒检查一次所有交易时间段的状态，根据当前时间自动处理状态转换
 *          包括从初始状态到收盘作业完成的全部状态流转过程
 */
void StateMonitor::run()
{
	std::lock_guard<std::mutex> lock(_thread_mutex);
	if(_thrd == NULL && !_stopped.load(std::memory_order_relaxed))
	{
		// 创建监控线程
		_thrd.reset(new StdThread([this](){
			currentMonitorWorker = this;
			uint64_t lastTime = 0;
			auto failWorker = [this](const char* message) {
				{
					std::lock_guard<std::mutex> stateLock(_state_mutex);
					_worker_failed = true;
				}
				_stopped.store(true, std::memory_order_relaxed);
				std::fprintf(stderr, "StateMonitor worker stopped after exception: %s\n", message);
			};

			// 主循环，直到收到停止信号
			try
			{
			while (!_stopped.load(std::memory_order_relaxed))
			{
				// 控制检查频率为每秒一次
				while(true)
				{
					uint64_t now = TimeUtils::getLocalTimeNow(); // 获取当前时间戳
					if(now - lastTime >= 1000) // 时间间隔达到1秒
						break;

					if(_stopped.load(std::memory_order_relaxed)) // 如果收到停止信号，立即退出
						break;

					// 短暂休眠，避免占用过多CPU资源
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}

				// 检查是否需要退出线程
				if(_stopped.load(std::memory_order_relaxed))
					break;

				// 获取当前日期和分钟数
				uint32_t curDate = TimeUtils::getCurDate(); // 当前日期，格式YYYYMMDD
				uint32_t curMin = TimeUtils::getCurMin() / 100; // 当前分钟数，格式HHMM

				advanceOnce(curDate, curMin);
				// 更新上次检查时间
				lastTime = TimeUtils::getLocalTimeNow();

				// 如果所有非节假日的交易时间段都处于收盘作业中状态，进行缓存清理
				if (isAllInState(SS_PROCING) && !isAllInState(SS_Holiday))
				{
					// 清理缓存数据，避免内存泄漏
					_dt_mgr->transHisData("CMD_CLEAR_CACHE");
				}
			} // 主循环结束
			}
			catch (const std::exception& ex)
			{
				failWorker(ex.what());
			}
			catch (...)
			{
				failWorker("unknown exception");
			}
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
	_stopped.store(true, std::memory_order_relaxed);  // 仅通知循环退出；join 提供线程结束同步

	// Do not wait on a mutex held by an external stop that is joining this
	// callback's worker. The owner must later call stop from another thread.
	if (currentMonitorWorker == this)
		return;
	std::lock_guard<std::mutex> lock(_thread_mutex);
	if (_thrd && _thrd->joinable())
		_thrd->join();  // 等待线程执行完成并安全退出
}
