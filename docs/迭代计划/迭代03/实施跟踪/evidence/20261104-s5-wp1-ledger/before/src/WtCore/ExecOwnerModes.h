/*!
 * \file ExecOwnerModes.h
 * \brief Per-executer owner identity and per-contract ownership mode (cards 5b/5c).
 *
 * An executer is one order owner ("exec:<name>"). For every contract it
 * records whether the execution unit's plugin advertises
 * WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY. Units without it run in
 * non-shared owner mode (exclusive account+contract claim) and never get
 * automatic timeout cancels enabled.
 */
#pragma once

#include "../Includes/WTSMarcos.h"

#include <mutex>
#include <string>
#include <unordered_map>

NS_WTP_BEGIN

class ExecOwnerModes
{
public:
	static std::string ownerId(const char* executerName)
	{
		return std::string("exec:") + (executerName ? executerName : "");
	}

	void setSharedCapable(const std::string& stdCode, bool capable)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_shared_capable[stdCode] = capable;
	}

	bool sharedCapable(const char* stdCode) const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		auto it = _shared_capable.find(stdCode ? stdCode : "");
		return it != _shared_capable.end() && it->second;
	}

private:
	mutable std::mutex _mutex;
	std::unordered_map<std::string, bool> _shared_capable;
};

NS_WTP_END
