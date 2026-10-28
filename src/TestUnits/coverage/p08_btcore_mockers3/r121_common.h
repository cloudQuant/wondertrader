/**
 * r121_common.h  (lane R121)
 *
 * 共享夹具：
 *   - r121::NullSink：仅用于满足 HisDataReplayer::prepare() 对 _listener 的非空要求，
 *     不接收任何断言逻辑；
 *   - r121::make_module_cfg：只含 module 字段的策略/执行器工厂配置，用于覆盖
 *     各 Mocker 的「插件加载成功、但不创建策略/执行单元」分支；
 *   - r121::read_text / line_count：复用 r62 的实现（r111_common.h 已包含）。
 *
 * 只使用公开 API；不 #define private public、不镜像类布局、不改宏。
 * 临时文件由 r62 fixture 写入系统临时目录并在 TearDown 清理。
 */
#pragma once

#include <gtest/gtest.h>

#include <string>

#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HisDataReplayer.h>

#include "r111_common.h"

namespace r121
{
	class NullSink : public IDataSink
	{
	public:
		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
		void handle_schedule(uint32_t, uint32_t) override {}
		void handle_init() override {}
		void handle_session_begin(uint32_t) override {}
		void handle_session_end(uint32_t) override {}
	};

	inline WTSVariant* make_module_cfg(const std::string& module)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());
		return cfg;
	}
}
