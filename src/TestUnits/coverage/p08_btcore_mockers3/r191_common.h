/**
 * r191_common.h  (lane R191)
 *
 * 只读复用 r111_common.h（插件路径 + 纯内存 TickLoader）与 r62_mocker_common.h
 * （真实 HisDataReplayer + 系统临时目录 + csv 日线），并补两个插件工厂配置。
 *
 * 所有临时文件只落系统临时目录；不联网、不起线程、不读挂钟。
 * 只使用公开 API：不 #define private public、不镜像类布局、不改宏。
 */
#pragma once

#include <string>

#include <Includes/WTSVariant.hpp>

#include "r111_common.h"

namespace r191
{
	// UftMocker::init_uft_factory 的最小合法配置：本机构建产物 WtUftRunner/libWtUftStraFact
	inline WTSVariant* make_uft_cfg(const std::string& module, const char* id, const char* code)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());

		WTSVariant* stra = WTSVariant::createObject();
		stra->append("name", "SimpleUft");
		stra->append("id", id);

		WTSVariant* params = WTSVariant::createObject();
		params->append("code", code);
		params->append("second", static_cast<uint32_t>(10));
		params->append("freq", static_cast<uint32_t>(0));
		params->append("offset", static_cast<uint32_t>(0));
		params->append("lots", 1.0);
		stra->append("params", params);
		params->release();

		cfg->append("strategy", stra);
		stra->release();
		return cfg;
	}

	// CtaMocker::init_cta_factory 的最小合法配置：本机构建产物 libWtCtaStraFact + DualThrust
	inline WTSVariant* make_cta_cfg(const std::string& module, const char* id, const char* code)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());

		WTSVariant* stra = WTSVariant::createObject();
		stra->append("name", "DualThrust");
		stra->append("id", id);

		WTSVariant* params = WTSVariant::createObject();
		params->append("days", static_cast<uint32_t>(2));
		params->append("k1", 0.5);
		params->append("k2", 0.5);
		params->append("period", "d");
		params->append("count", static_cast<uint32_t>(5));
		params->append("code", code);
		params->append("stock", false);
		stra->append("params", params);
		params->release();

		cfg->append("strategy", stra);
		stra->release();
		return cfg;
	}
}
