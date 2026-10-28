/*!
 * \file r45_fake_parserapi.h
 * \brief R45 lane 自用的 IParserApi 替身（仅测试侧，不进产品代码）
 *
 * ParserAdapter::initExt(id, api) 是产品公开 API，接受外部 IParserApi*；
 * 用它可以在不加载任何插件、不建 socket/线程的前提下驱动
 * ParserAdapter.cpp:44-79（registerSpi / init / 合约集合 / subscribe）与
 * :219-226（run -> connect）、:205-217（release -> api->release + delete）。
 *
 * 静态计数用于「对象已被 release 删除后」仍可观测调用次数。
 */
#pragma once

#include <Includes/IParserApi.h>
#include <Includes/WTSVariant.hpp>

namespace r45 {

class FakeParserApi : public wtp::IParserApi
{
public:
	explicit FakeParserApi(bool initOk = true, bool connectOk = true)
		: _initOk(initOk), _connectOk(connectOk)
	{
	}

	~FakeParserApi() override { ++s_deleted; }

	bool init(wtp::WTSVariant* config) override
	{
		++init_calls;
		last_cfg = config;
		return _initOk;
	}

	void release() override
	{
		++release_calls;
		++s_release_calls;
	}

	bool connect() override
	{
		++connect_calls;
		return _connectOk;
	}

	bool disconnect() override
	{
		++disconnect_calls;
		return true;
	}

	bool isConnected() override { return _connectOk && connect_calls > 0; }

	void subscribe(const wtp::CodeSet& setCodes) override
	{
		++subscribe_calls;
		subscribed = setCodes;
	}

	void unsubscribe(const wtp::CodeSet& /*setCodes*/) override { ++unsubscribe_calls; }

	void registerSpi(IParserSpi* spi) override
	{
		++register_calls;
		spi_ptr = spi;
	}

	static void resetCounters() { s_deleted = 0; s_release_calls = 0; }

	static int s_deleted;
	static int s_release_calls;

	int init_calls = 0;
	int release_calls = 0;
	int connect_calls = 0;
	int disconnect_calls = 0;
	int subscribe_calls = 0;
	int unsubscribe_calls = 0;
	int register_calls = 0;

	wtp::CodeSet subscribed;
	IParserSpi* spi_ptr = nullptr;
	wtp::WTSVariant* last_cfg = nullptr;

private:
	bool _initOk;
	bool _connectOk;
};

}  // namespace r45
