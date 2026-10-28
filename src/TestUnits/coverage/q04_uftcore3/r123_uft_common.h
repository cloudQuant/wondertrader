/*!
 * \file r123_uft_common.h
 * \brief 第二十波 R123 - WtUftCore 测试共用夹具（只给 r123_* 使用）
 *
 * 只提供纯内存替身与构建产物定位：
 *  - SessionBaseDataMgr：固定返回一个 WTSSessionInfo，供 WtUftEngine/WtUftRtTicker 使用；
 *  - exeDir/findLib：从测试可执行体所在目录向上找构建出的插件；
 *  - makeTick：构造一条 SHFE.ag2406 的 tick。
 *
 * 不连接柜台、不建 socket/shared memory、不加载任何交易/行情插件。
 */
#pragma once

#include "q04_trader_fixture.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"

#include <mach-o/dyld.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <string>

namespace r123 {

using namespace wtp;

// 固定返回同一个交易时段的假基础数据管理器（合约/品种仍由 q04 替身维护）
class SessionBaseDataMgr : public q04::FakeBaseDataMgr
{
public:
	WTSSessionInfo*	_session = nullptr;

	WTSSessionInfo* getSession(const char* /*sid*/) override { return _session; }
	WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/ = "") override
	{
		return _session;
	}
};

inline std::string exeDir()
{
	char buf[4096] = { 0 };
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) != 0)
		return std::string();

	std::error_code ec;
	auto p = std::filesystem::weakly_canonical(std::filesystem::path(buf), ec);
	if (ec)
		return std::string();
	return p.parent_path().string();
}

// 插件的实际位置随构建目录而变：<bin>/、<bin>/../、<bin>/TestUnits/../ 都试一遍
inline std::string findLib(const char* name)
{
	const std::string dir = exeDir();
	if (dir.empty())
		return std::string();

	const std::filesystem::path base(dir);
	const std::filesystem::path candidates[] = {
		base / name,
		base.parent_path() / name,
		base.parent_path().parent_path() / name,
		base.parent_path().parent_path().parent_path() / name,
	};

	for (const auto& one : candidates)
	{
		std::error_code ec;
		if (!std::filesystem::exists(one, ec))
			continue;

		ec.clear();
		const std::string ret = std::filesystem::weakly_canonical(one, ec).string();
		if (!ec)
			return ret;
	}
	return std::string();
}

inline WTSTickData* makeTick(uint32_t date, uint32_t hhmmss, double price,
	const char* stdCode = "SHFE.ag2406")
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.exchg, "SHFE");
	wt_strcpy(ts.code, stdCode);
	ts.action_date = date;
	ts.action_time = hhmmss;
	ts.trading_date = date;
	ts.price = price;
	return WTSTickData::create(ts);
}

}	// namespace r123
