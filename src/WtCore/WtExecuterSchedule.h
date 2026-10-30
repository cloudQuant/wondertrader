#pragma once

#include "../Includes/FasterDefs.h"

NS_WTP_BEGIN
namespace WtExecuterSchedule
{

/**
 * Schedule a channel-drain task, running it synchronously if the pool rejects it.
 * The caller must invoke this only after releasing executor/state locks.
 */
template <typename Pool, typename Task, typename Fallback>
bool scheduleOrRunFallback(Pool& pool, Task task, Fallback fallback)
{
	if (pool.schedule(task))
		return true;
	fallback();
	return false;
}
}

NS_WTP_END
