#pragma once
// Shared helpers for the dt-writer lifecycle fixture (iteration 03, 2026-11-04, package dt-writer).
#include <gtest/gtest.h>

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IDataWriter.h"
#include "WtDataStorage/DataDefine.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace s5 {

namespace fs = std::filesystem;
using namespace wtp;

inline std::string module_dir()
{
	const char* dir = std::getenv("S5_MODULE_DIR");
	std::string value = dir == nullptr ? "" : dir;
	if (!value.empty() && value.back() != '/')
		value.push_back('/');
	return value;
}

inline fs::path unique_dir(const char* tag)
{
	static std::atomic<unsigned> sequence{ 0 };
	fs::path dir = fs::current_path() / (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(sequence++));
	fs::create_directories(dir);
	return dir;
}

inline std::vector<std::string> read_lines(const fs::path& path)
{
	std::vector<std::string> lines;
	std::ifstream input(path.string());
	std::string line;
	while (std::getline(input, line))
		lines.push_back(line);
	return lines;
}

inline long index_of(const std::vector<std::string>& lines, const std::string& value)
{
	for (size_t i = 0; i < lines.size(); i++)
	{
		if (lines[i] == value)
			return (long)i;
	}
	return -1;
}

inline size_t count_of(const std::vector<std::string>& lines, const std::string& value)
{
	size_t n = 0;
	for (const auto& line : lines)
		n += (line == value) ? 1 : 0;
	return n;
}

// One futures contract with a 09:00-15:00 session; owns every object it creates.
struct Market
{
	WTSSessionInfo*		session = nullptr;
	WTSCommodityInfo*	commodity = nullptr;
	WTSContractInfo*	contract = nullptr;

	Market()
	{
		session = WTSSessionInfo::create("S5DAY", "s5 day", 0);
		session->addTradingSection(900, 1500);
		commodity = WTSCommodityInfo::create("rb", "s5 rb", "SHFE", "S5DAY", "CHN");
		commodity->setSessionInfo(session);
		commodity->setCategory(CC_Future);
		commodity->addCode("rb2210");
		contract = WTSContractInfo::create("rb2210", "s5 rb2210", "SHFE", "rb");
		contract->setCommInfo(commodity);
	}

	~Market()
	{
		contract->release();
		commodity->release();
		session->release();
	}

	WTSTickData* tick(uint32_t seq) const
	{
		WTSTickStruct raw;
		std::memset(&raw, 0, sizeof(raw));
		std::strcpy(raw.exchg, "SHFE");
		std::strcpy(raw.code, "rb2210");
		raw.trading_date = 20260105;
		raw.action_date = 20260105;
		raw.action_time = 93000000 + (seq % 50) * 1000;
		raw.price = 3500;
		raw.open = raw.high = raw.low = 3500;
		raw.total_volume = 100;
		raw.volume = 1;
		WTSTickData* data = WTSTickData::create(raw);
		data->setContractInfo(contract);
		return data;
	}

	WTSOrdQueData* queue(uint32_t seq) const
	{
		WTSOrdQueStruct raw;
		std::memset(&raw, 0, sizeof(raw));
		std::strcpy(raw.exchg, "SHFE");
		std::strcpy(raw.code, "rb2210");
		raw.trading_date = 20260105;
		raw.action_date = 20260105;
		raw.action_time = 93000000 + seq;
		WTSOrdQueData* data = WTSOrdQueData::create(raw);
		data->setContractInfo(contract);
		return data;
	}

	WTSOrdDtlData* detail(uint32_t seq) const
	{
		WTSOrdDtlStruct raw;
		std::memset(&raw, 0, sizeof(raw));
		std::strcpy(raw.exchg, "SHFE");
		std::strcpy(raw.code, "rb2210");
		raw.trading_date = 20260105;
		raw.action_date = 20260105;
		raw.action_time = 93000000 + seq;
		WTSOrdDtlData* data = WTSOrdDtlData::create(raw);
		data->setContractInfo(contract);
		return data;
	}

	WTSTransData* trans(uint32_t seq) const
	{
		WTSTransStruct raw;
		std::memset(&raw, 0, sizeof(raw));
		std::strcpy(raw.exchg, "SHFE");
		std::strcpy(raw.code, "rb2210");
		raw.trading_date = 20260105;
		raw.action_date = 20260105;
		raw.action_time = 93000000 + seq;
		WTSTransData* data = WTSTransData::create(raw);
		data->setContractInfo(contract);
		return data;
	}
};

inline uint32_t action_time_of(WTSObject* obj, int stream)
{
	switch (stream)
	{
	case 1: return ((WTSOrdQueData*)obj)->getOrdQueStruct().action_time;
	case 2: return ((WTSOrdDtlData*)obj)->getOrdDtlStruct().action_time;
	case 3: return ((WTSTransData*)obj)->getTransStruct().action_time;
	default: return 0;
	}
}

// Published record count of an RT day block (header _size), 0 when absent.
inline uint32_t rt_block_size(const fs::path& path)
{
	RTDayBlockHeader header;
	std::memset(&header, 0, sizeof(header));
	std::ifstream input(path.string(), std::ios::binary);
	if (!input.read(reinterpret_cast<char*>(&header), sizeof(header)))
		return 0;
	return header._size;
}

} // namespace s5
