// Per-family test harness for the vendor parser V2 fixture. Select one of
// FAMILY_CTP, FAMILY_CTPMINI, FAMILY_CTPOPT, FAMILY_FEMAS, FAMILY_XTP and pass
// PARSER_MODULE (absolute path of the parser dylib under test) and FAKE_LIB
// (absolute path of the fake vendor SDK library that parser uses).
#pragma once

#include <gtest/gtest.h>

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/IParserLifecycleV2.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;
using namespace std::chrono_literals;

namespace vfx
{
	enum Counter { C_CREATED, C_INIT, C_RELEASE, C_RELEASE_ON_SDK_THREAD, C_RELEASE_DURING_CALLBACK, C_SPI_NULL,
		C_DISPATCHED, C_LATE_DISPATCHED, C_LOGIN_REQ, C_SUB_REQ, C_LOGOUT };
	enum Option { OPT_CREATE_NULL = 1, OPT_LATE_CALLBACK = 2, OPT_BLOCK_SUBSCRIBE = 3, OPT_LOGIN_RESULT = 4 };
	enum Kind { K_FRONT_CONNECTED = 1, K_LOGIN = 2, K_DEPTH = 3, K_DISCONNECTED = 4 };

#if defined(FAMILY_CTP)
	constexpr const char* kName = "ParserCTP";
	constexpr bool kXtp = false;
	constexpr const char* kExchg = "SHFE";
	constexpr const char* kCode = "ag2512";
#elif defined(FAMILY_CTPMINI)
	constexpr const char* kName = "ParserCTPMini";
	constexpr bool kXtp = false;
	constexpr const char* kExchg = "SHFE";
	constexpr const char* kCode = "ag2512";
#elif defined(FAMILY_CTPOPT)
	constexpr const char* kName = "ParserCTPOpt";
	constexpr bool kXtp = false;
	constexpr const char* kExchg = "SHFE";
	constexpr const char* kCode = "ag2512";
#elif defined(FAMILY_FEMAS)
	constexpr const char* kName = "ParserFemas";
	constexpr bool kXtp = false;
	constexpr const char* kExchg = "SHFE";
	constexpr const char* kCode = "ag2512";
#elif defined(FAMILY_XTP)
	constexpr const char* kName = "ParserXTP";
	constexpr bool kXtp = true;
	constexpr const char* kExchg = "SSE";
	constexpr const char* kCode = "600000";
#else
#error "select a parser family"
#endif

	struct Fake
	{
		void* handle = dlopen(FAKE_LIB, RTLD_NOW | RTLD_GLOBAL);
		int (*counterFn)(int) = handle ? reinterpret_cast<int (*)(int)>(dlsym(handle, "fake_md_counter")) : nullptr;
		void (*resetFn)() = handle ? reinterpret_cast<void (*)()>(dlsym(handle, "fake_md_reset")) : nullptr;
		void (*setFn)(int, int) = handle ? reinterpret_cast<void (*)(int, int)>(dlsym(handle, "fake_md_set")) : nullptr;
		int (*postFn)(int) = handle ? reinterpret_cast<int (*)(int)>(dlsym(handle, "fake_md_post")) : nullptr;
		int (*idleFn)(int) = handle ? reinterpret_cast<int (*)(int)>(dlsym(handle, "fake_md_wait_idle")) : nullptr;
		int (*onSdkFn)() = handle ? reinterpret_cast<int (*)()>(dlsym(handle, "fake_md_on_sdk_thread")) : nullptr;
		int (*subEnteredFn)(int) = handle ? reinterpret_cast<int (*)(int)>(dlsym(handle, "fake_md_wait_subscribe_entered")) : nullptr;
		void (*unblockFn)() = handle ? reinterpret_cast<void (*)()>(dlsym(handle, "fake_md_unblock_subscribe")) : nullptr;
		void (*probeFn)(int (*)()) = handle ? reinterpret_cast<void (*)(int (*)())>(dlsym(handle, "fake_md_set_release_probe")) : nullptr;

		bool ok() const { return counterFn && resetFn && setFn && postFn && idleFn && onSdkFn && subEnteredFn && unblockFn && probeFn; }
		void setReleaseProbe(int (*probe)()) const { probeFn(probe); }
		int count(int c) const { return counterFn(c); }
		void reset() const { resetFn(); }
		void set(int o, int v) const { setFn(o, v); }
		bool post(int k) const { return postFn(k) != 0; }
		bool idle(int ms = 2000) const { return idleFn(ms) != 0; }
		bool onSdkThread() const { return onSdkFn() != 0; }
		bool subscribeEntered(int ms = 2000) const { return subEnteredFn(ms) != 0; }
		void unblockSubscribe() const { unblockFn(); }
	};
	inline Fake& fake() { static Fake f; return f; }

	struct Module
	{
		explicit Module(const char* path = PARSER_MODULE) : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {}
		void* handle;
		FuncCreateParser create = handle ? reinterpret_cast<FuncCreateParser>(dlsym(handle, "createParser")) : nullptr;
		FuncDeleteParser remove = handle ? reinterpret_cast<FuncDeleteParser>(dlsym(handle, "deleteParser")) : nullptr;
		WtParserLifecycleV2GetterFn get = handle ? reinterpret_cast<WtParserLifecycleV2GetterFn>(dlsym(handle, WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL)) : nullptr;
		const WtParserLifecycleApiV2* table = get ? get(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR) : nullptr;
		// Quarantined parsers keep the module loaded on purpose: never dlclose.
	};

	class BaseData : public IBaseDataMgr
	{
	public:
		BaseData()
		{
			comm = WTSCommodityInfo::create(kXtp ? "STK" : "ag", "fixture", kExchg, "SESSION", "HOLIDAY");
			comm->setCategory(kXtp ? CC_Stock : CC_Future);
			comm->setVolScale(1);
			ct = WTSContractInfo::create(kCode, "fixture", kExchg, kXtp ? "STK" : "ag");
			ct->setCommInfo(comm);
		}
		~BaseData()
		{
			ct->release();
			comm->release();
		}
		WTSCommodityInfo* getCommodity(const char*) override { return comm; }
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return comm; }
		WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
		{
			return (code != nullptr && std::string(code) == kCode) ? ct : nullptr;
		}
		WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
		WTSSessionInfo* getSession(const char*) override { return nullptr; }
		WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
		WTSArray* getAllSessions() override { return nullptr; }
		bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
		uint32_t calcTradingDate(const char*, uint32_t date, uint32_t, bool = false) override { return date; }
		uint64_t getBoundaryTime(const char*, uint32_t date, bool = false, bool = true) override { return date; }

		WTSCommodityInfo* comm;
		WTSContractInfo* ct;
	};

	class Sink : public IParserSpi
	{
	public:
		BaseData base;
		std::function<void()> onQuote;
		std::function<void(WTSParserEvent)> onEvent;
		std::mutex mutex;
		std::condition_variable cv;
		int quotes = 0, connects = 0, logins = 0, closes = 0;
		bool blockQuotes = false, quoteEntered = false, quoteOpen = false;
		std::atomic<bool> inQuote{false};	// true while handleQuote is executing

		IBaseDataMgr* getBaseDataMgr() override { return &base; }
		void handleSymbolList(const WTSArray*) override {}
		void handleParserLog(WTSLogLevel, const char*) override {}
		void handleEvent(WTSParserEvent e, int32_t) override
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (e == WPE_Connect) ++connects;
				else if (e == WPE_Login) ++logins;
				else if (e == WPE_Close) ++closes;
				cv.notify_all();
			}
			if (onEvent)
				onEvent(e);
		}
		void handleQuote(WTSTickData*, uint32_t) override
		{
			inQuote.store(true);
			struct Leave { std::atomic<bool>& flag; ~Leave() { flag.store(false); } } leave{inQuote};
			bool block;
			{
				std::lock_guard<std::mutex> lock(mutex);
				++quotes;
				quoteEntered = true;
				block = blockQuotes;
				cv.notify_all();
			}
			if (onQuote)
				onQuote();
			if (block)
			{
				std::unique_lock<std::mutex> lock(mutex);
				cv.wait_for(lock, 5s, [this]() { return quoteOpen; });
			}
		}
		int quoteCount() { std::lock_guard<std::mutex> l(mutex); return quotes; }
		int eventCount(WTSParserEvent e)
		{
			std::lock_guard<std::mutex> l(mutex);
			return e == WPE_Connect ? connects : (e == WPE_Login ? logins : closes);
		}
		bool waitQuoteEntered(std::chrono::milliseconds t = 2s)
		{
			std::unique_lock<std::mutex> l(mutex);
			return cv.wait_for(l, t, [this]() { return quoteEntered; });
		}
		bool waitEvents(WTSParserEvent e, int n, std::chrono::milliseconds t = 2s)
		{
			std::unique_lock<std::mutex> l(mutex);
			return cv.wait_for(l, t, [&]() { return (e == WPE_Connect ? connects : (e == WPE_Login ? logins : closes)) >= n; });
		}
		void openQuotes()
		{
			std::lock_guard<std::mutex> l(mutex);
			quoteOpen = true;
			cv.notify_all();
		}
	};

	// Probe for the fake SDK: is the observed sink still inside handleQuote?
	inline std::atomic<Sink*>& probedSink() { static std::atomic<Sink*> sink{nullptr}; return sink; }
	inline int sinkInQuote()
	{
		Sink* sink = probedSink().load();
		return (sink != nullptr && sink->inQuote.load()) ? 1 : 0;
	}
	inline void probeSink(Sink& sink)
	{
		probedSink().store(&sink);
		fake().setReleaseProbe(&sinkInQuote);
	}

	struct TempDir
	{
		std::string path;
		TempDir()
		{
			char tmpl[] = "/tmp/wt_i03_vendor_v2_XXXXXX";
			const char* made = mkdtemp(tmpl);
			path = made ? made : "";
		}
		~TempDir()
		{
			std::error_code ec;
			if (!path.empty())
				std::filesystem::remove_all(path, ec);
		}
	};

	// Configuration accepted by the parser under test; `module` adds the
	// ParserAdapter "module" key.
	inline WTSVariant* makeConfig(const std::string& flowdir, bool withModuleKey = false)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("user", "fixture");
		cfg->append("pass", "fixture");
		cfg->append("flowdir", flowdir.c_str());
#if defined(FAMILY_XTP)
		cfg->append("host", "127.0.0.1");
		cfg->append("port", 1);
		cfg->append("protocol", 1u);
		cfg->append("clientid", 1u);
		cfg->append("hbinterval", 15u);
		cfg->append("buffsize", 64u);
		cfg->append("local_ip", "127.0.0.1");
		cfg->append("xtpmodule", "fake_xtp_md");
#else
		cfg->append("front", "tcp://127.0.0.1:1");
		cfg->append("broker", "9999");
#if defined(FAMILY_CTPMINI)
		cfg->append("ctpmodule", "fake_ctpmini_md");
#elif defined(FAMILY_CTPOPT)
		cfg->append("ctpmodule", "fake_ctpopt_md");
#elif defined(FAMILY_FEMAS)
		cfg->append("ctpmodule", "fake_femas_md");
#endif
#endif
		if (withModuleKey)
			cfg->append("module", kName);
		return cfg;
	}

	inline CodeSet subscription()
	{
		CodeSet codes;
		codes.insert(std::string(kExchg) + "." + kCode);
		return codes;
	}

	// connect() and, for the CTP family, drive front-connected + login on the
	// fake SDK thread so the parser reaches its logged-in state.
	inline bool bringOnline(IParserApi* api)
	{
		if (!api->connect())
			return false;
		if (!kXtp)
		{
			if (!fake().post(K_FRONT_CONNECTED) || !fake().post(K_LOGIN))
				return false;
		}
		return fake().idle();
	}

	inline bool postAndWait(int kind)
	{
		return fake().post(kind) && fake().idle();
	}
}
