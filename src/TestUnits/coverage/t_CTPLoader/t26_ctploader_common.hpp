/*
 * t26_ctploader_common.hpp — lane R26 的公共夹具
 *
 * 只做三件事：
 *  1) 把 CTPLoader/TraderSpi 两个 TU 里的「全局状态」以 extern 声明暴露给用例
 *     （pUserApi / BROKER_ID / CLASSMASK / _contracts / _commodities / _fees / _margins 等，
 *      它们都在 src/CTPLoader 目录下两个 cpp 的命名空间作用域定义，非 static，可直接引用）；
 *  2) 提供 std::cerr 内存捕获、临时目录、轮询等待三个小工具；
 *  3) 给出统一的 setup_defaults()：每个用例先把厂商全局重置成「本地可控」的样子。
 *
 * 注意：门禁按「一条用例一个进程」跑（case_runner），所以 fillCzceCode 的 static 锁存、
 * 未跑 run() 前不确定的 CLASSMASK/INCREMENTAL 等，都在 setup_defaults() 里显式置定。
 */
#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "CTPLoader/TraderSpi.h"

USING_NS_WTP;

/* ---------------- 生产全局（src/CTPLoader/CTPLoader.cpp / TraderSpi.cpp） ---------------- */
extern CThostFtdcTraderApi* pUserApi;			// CTPLoader.cpp L27

extern std::string FRONT_ADDR;				// CTPLoader.cpp L30-44
extern std::string BROKER_ID;
extern std::string INVESTOR_ID;
extern std::string PASSWORD;
extern std::string SAVEPATH;
extern std::string APPID;
extern std::string AUTHCODE;
extern uint32_t CLASSMASK;
extern bool ONLYINCFG;
extern bool INCREMENTAL;
extern bool QRYFEES;
extern std::string COMM_FILE;
extern std::string CONT_FILE;
extern std::string FEES_FILE;

extern std::map<std::string, std::string> MAP_NAME;		// CTPLoader.cpp L49-53
extern std::map<std::string, std::string> MAP_SESSION;
extern std::set<std::string> SET_FILTERS;

extern int iRequestID;						// CTPLoader.cpp L59

extern TThostFtdcFrontIDType FRONT_ID;			// TraderSpi.cpp L72-74
extern TThostFtdcSessionIDType SESSION_ID;
extern TThostFtdcOrderRefType ORDER_REF;

extern loader::CommodityMap _commodities;		// TraderSpi.cpp L76-79
extern loader::ContractMap _contracts;
extern loader::FeeMap _fees;
extern loader::MarginMap _margins;

/* 自由函数：TraderSpi.cpp L303-340（内部有 yearFlag 的 static 锁存，见用例注释） */
std::string fillCzceCode(const char* code, uint32_t tdate);

namespace t26
{
	/// std::cerr 内存捕获：胶水层的可观察通道（仅限「无并发写 stderr」的用例）
	class CoutCapture
	{
	public:
		CoutCapture() : _old(std::cerr.rdbuf()) { std::cerr.rdbuf(_ss.rdbuf()); }
		~CoutCapture() { std::cerr.rdbuf(_old); }

		std::string text() const { return _ss.str(); }

	private:
		std::ostringstream _ss;
		std::streambuf* _old;
	};

	/// 整文件回读（落盘断言 + FdRedirectGuard 日志共用）
	inline std::string read_whole_file(const std::filesystem::path& p)
	{
		std::ifstream ifs(p.string(), std::ios::binary);
		std::stringstream ss;
		ss << ifs.rdbuf();
		return ss.str();
	}

	/*
	 * fd 级 stderr 重定向：凡「起过 AppendQuery worker」的用例**必须**用它，不能用 CoutCapture。
	 * 实测教训（stress 40/1 复现）：worker 的 `std::cerr << ...` 与用例主线程的 rdbuf 换入/换出
	 * 并发进行 → 两个线程同时往同一个 ostringstream 写 → libc++ 堆破坏（malloc 报
	 * "pointer being freed was not allocated" → SIGABRT/SIGTRAP，崩溃点随调度漂移）。
	 * dup2 换的是底层 fd，写方各用各的 streambuf，线程安全；代价是文本要靠回读临时文件。
	 */
	class FdRedirectGuard
	{
	public:
		explicit FdRedirectGuard(const std::string& path) : _path(path)
		{
			fflush(stderr);
			_saved = dup(STDERR_FILENO);
			int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd >= 0)
			{
				dup2(fd, STDERR_FILENO);
				close(fd);
			}
		}
		~FdRedirectGuard()
		{
			fflush(stderr);
			if (_saved >= 0)
				dup2(_saved, STDERR_FILENO);
		}

		/// worker 静默之后回读日志（wait_for + sleep 之后再调用）
		std::string read_log() const
		{
			std::string text = t26::read_whole_file(_path);
			return text;
		}

	private:
		std::string _path;
		int _saved = -1;
	};

	/// 把定长厂商字符数组安全地填成测试值
	inline void set_fixed(char* dst, size_t cap, const std::string& src)
	{
		size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
		std::memcpy(dst, src.data(), n);
		dst[n] = '\0';
	}

	/// 轮询等待（worker 线程消费队列有 1010ms 限速，见 TraderSpi.cpp L248-255）
	template <typename Pred>
	inline bool wait_for(Pred pred, int timeout_ms = 15000)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (pred())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return pred();
	}

	/// 每个用例的统一前置：把「run() 才会置定」的全局全部显式化，避免不确定值
	inline void setup_defaults(const std::string& uniq = "t26")
	{
		pUserApi = NULL;

		FRONT_ADDR = "tcp://127.0.0.1:1";
		BROKER_ID = "9999";
		INVESTOR_ID = "88888";
		PASSWORD = "secret";
		APPID = "t26.app";
		AUTHCODE = "t26.auth";

		CLASSMASK = 1;		// 默认只要期货
		ONLYINCFG = false;
		INCREMENTAL = false;
		QRYFEES = false;

		iRequestID = 0;
		FRONT_ID = 0;
		SESSION_ID = 0;
		std::memset(ORDER_REF, 0, sizeof(ORDER_REF));

		_commodities.clear();
		_contracts.clear();
		_fees.clear();
		_margins.clear();
		MAP_NAME.clear();
		MAP_SESSION.clear();
		SET_FILTERS.clear();

		// 落盘类用例统一写到 /tmp 下的独立目录（尾斜杠：产品是 SAVEPATH+文件名 的直接拼接）
		std::error_code ec;
		std::filesystem::path dir = std::filesystem::temp_directory_path() / ("wtdump_" + uniq);
		std::filesystem::remove_all(dir, ec);
		std::filesystem::create_directories(dir, ec);
		SAVEPATH = dir.string() + "/";
		COMM_FILE = "commodities.json";
		CONT_FILE = "contracts.json";
		FEES_FILE = "fees.json";
	}

	inline CThostFtdcRspInfoField ok_rsp()
	{
		CThostFtdcRspInfoField rsp;
		std::memset(&rsp, 0, sizeof(rsp));
		rsp.ErrorID = 0;
		return rsp;
	}

	inline CThostFtdcRspInfoField err_rsp(int id, const std::string& msg = "t26-error")
	{
		CThostFtdcRspInfoField rsp;
		std::memset(&rsp, 0, sizeof(rsp));
		rsp.ErrorID = id;
		set_fixed(rsp.ErrorMsg, sizeof(rsp.ErrorMsg), msg);
		return rsp;
	}

	/// 造一条 CThostFtdcInstrumentField（未用到的字段保持 0 填充）
	inline CThostFtdcInstrumentField make_instrument(const std::string& exchg, const std::string& inst,
		const std::string& product, char pClass)
	{
		CThostFtdcInstrumentField f;
		std::memset(&f, 0, sizeof(f));
		set_fixed(f.ExchangeID, sizeof(f.ExchangeID), exchg);
		set_fixed(f.InstrumentID, sizeof(f.InstrumentID), inst);
		set_fixed(f.ProductID, sizeof(f.ProductID), product);
		f.ProductClass = pClass;
		f.LongMarginRatio = 0.1;
		f.ShortMarginRatio = 0.12;
		f.VolumeMultiple = 10;
		f.PriceTick = 1.0;
		f.MaxMarketOrderVolume = 500;
		f.MinMarketOrderVolume = 1;
		f.MaxLimitOrderVolume = 1000;
		f.MinLimitOrderVolume = 2;
		set_fixed(f.OpenDate, sizeof(f.OpenDate), "20170101");
		set_fixed(f.ExpireDate, sizeof(f.ExpireDate), "20251015");
		return f;
	}
}
