/*!
 * \file r102_seed.h
 * \brief R102 lane 自用夹具：临时根目录、基础数据文件、WtDataStorage 历史 .dsb 数据
 *
 * 只给 q05b_dtservo/r102_*.cpp 使用，不改产品代码、不加载真实共享内存/socket。
 *
 * 历史数据文件格式（`his/<pname>/<exchg>/<code>.dsb`、`his/ticks/<exchg>/<date>/<code>.dsb`）：
 *   BlockHeader(12B, flag="&^%$#@!\0", version=BLOCK_VERSION_RAW_V2=3) + 原始结构体数组
 *   `WtRdmDtReader::cacheHisBarsFromFile` / `readTickSliceByDate` 对 version=3 走
 *   `proc_block_data(..., bKeepHead=false)`，只剥掉 12 字节头，不压缩、不换结构体。
 *
 * 所有写入都落系统临时目录；不给任何 reader 起线程、不 sleep（真实 reader 的 5s 巡检
 * 线程只会在 fork 子进程内出现，子进程完成查询后立即 exit，父进程无残留）。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <dlfcn.h>

#include <filesystem>

#include <Includes/WTSMarcos.h>
#include <Includes/WTSStruct.h>
#include <Share/TimeUtils.hpp>

namespace r102 {

namespace fs = std::filesystem;

// WtDataStorage/DataDefine.h 的 BlockHeader 在 pack(1) 下固定 12 字节。
struct BlkHeader
{
	char		_blk_flag[8];
	uint16_t	_type;
	uint16_t	_version;
};
static_assert(sizeof(BlkHeader) == 12, "BlockHeader must stay 12 bytes");

enum : uint16_t
{
	BT_HIS_Minute1 = 21,
	BT_HIS_Minute5 = 22,
	BT_HIS_Day = 23,
	BT_HIS_Ticks = 24
};

inline fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r102_") + tag + "_" + std::to_string(::getpid()) + "_"
			+ std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

inline void write_text(const fs::path& p, const std::string& content)
{
	fs::create_directories(p.parent_path());
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

inline std::string with_slash(const fs::path& p)
{
	std::string s = p.string();
	if (!s.empty() && s.back() != '/')
		s.push_back('/');
	return s;
}

/*!
 * 由测试可执行体自身路径推模块目录：<bin>/TestUnits/CoverageTests_* -> <bin>/
 */
inline std::string bin_dir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&bin_dir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();

	fs::path bin = fs::path(info.dli_fname).parent_path().parent_path();
	return with_slash(bin);
}

// 插件存在且导出 create/delete 两个符号时才算可用；否则调用方 GTEST_SKIP，
// 绝不把「插件缺失」退化成 initStore 里的 _reader 空指针解引用。
inline bool storage_plugin_ready(const std::string& bin)
{
	if (bin.empty())
		return false;

	const std::string path = bin + "libWtDataStorage.dylib";
	std::error_code ec;
	if (!fs::exists(path, ec))
		return false;

	void* h = dlopen(path.c_str(), RTLD_NOW);
	if (h == nullptr)
		return false;

	const bool ok = dlsym(h, "createRdmDtReader") != nullptr
		&& dlsym(h, "deleteRdmDtReader") != nullptr;
	dlclose(h);
	return ok;
}

inline bool ad_writer_plugin_ready(const std::string& bin)
{
	if (bin.empty())
		return false;

	const std::string path = bin + "libWtDataStorageAD.dylib";
	std::error_code ec;
	if (!fs::exists(path, ec))
		return false;

	void* h = dlopen(path.c_str(), RTLD_NOW);
	if (h == nullptr)
		return false;

	const bool ok = dlsym(h, "createWriter") != nullptr
		&& dlsym(h, "deleteWriter") != nullptr;
	dlclose(h);
	return ok;
}

/*!
 * 基础数据：一条 09:30-15:00 日盘、一个期货品种 CFFEX.IC、一个股票 SSE.600000。
 * 600000 用来驱动 WtRdmDtReader::readTickSliceByCount（该函数用 _code 当 product 查表，
 * 股票代码与 product 相同，因此不会落入 R23 已登记的期货空指针缺陷）。
 */
inline void write_basefiles(const fs::path& dir)
{
	write_text(dir / "sessions.json",
		R"({"SD0930":{"name":"r102 day","offset":0,"auction":{"from":929,"to":930},
		"sections":[{"from":930,"to":1500}]}})");
	write_text(dir / "holidays.json", R"({"CHINA":["20240101"]})");

	write_text(dir / "commodities.json",
		std::string("{") +
		R"("CFFEX":{"IC":{"covermode":0,"pricemode":0,"category":1,)"
		R"("precision":1,"pricetick":0.2,"volscale":200,"name":"CSI500",)"
		R"("exchg":"CFFEX","session":"SD0930","holiday":"CHINA"}},)"
		R"("SSE":{"600000":{"covermode":0,"pricemode":0,"category":0,)"
		R"("precision":2,"pricetick":0.01,"volscale":100,"name":"SPDB",)"
		R"("exchg":"SSE","session":"SD0930","holiday":"CHINA"}}})");

	write_text(dir / "contracts.json",
		std::string("{") +
		R"("CFFEX":{"IC2204":{"name":"CSI500 2204","code":"IC2204",)"
		R"("exchg":"CFFEX","product":"IC","maxlimitqty":20,"maxmarketqty":10}},)"
		R"("SSE":{"600000":{"name":"SPDB","code":"600000","exchg":"SSE",)"
		R"("product":"600000","maxlimitqty":100000,"maxmarketqty":1000}}})");
}

// 不带 data 段：供 WtDataManager 单测构造「只有 base/hot manager」的 WtDtRunner。
inline std::string runner_cfg_json(const fs::path& dir)
{
	const std::string d = dir.string();
	return std::string("{\"disable_dump\":true,\"basefiles\":{")
		+ "\"session\":\"" + d + "/sessions.json\","
		+ "\"commodity\":\"" + d + "/commodities.json\","
		+ "\"contract\":\"" + d + "/contracts.json\","
		+ "\"holiday\":\"" + d + "/holidays.json\"}}";
}

inline std::string data_cfg_json(const std::string& storeWithSlash, bool withModule)
{
	std::string s = "{\"align_by_section\":false,\"store\":{";
	if (withModule)
		s += "\"module\":\"WtDataStorage\",";
	s += "\"path\":\"" + storeWithSlash + "\"}}";
	return s;
}

inline std::string full_cfg_json(const fs::path& dir, const std::string& storeWithSlash,
	bool withModule = true)
{
	const std::string d = dir.string();
	return std::string("{\"disable_dump\":true,\"basefiles\":{")
		+ "\"session\":\"" + d + "/sessions.json\","
		+ "\"commodity\":\"" + d + "/commodities.json\","
		+ "\"contract\":\"" + d + "/contracts.json\","
		+ "\"holiday\":\"" + d + "/holidays.json\"},"
		+ "\"data\":" + data_cfg_json(storeWithSlash, withModule) + "}";
}

inline WTSBarStruct make_day_bar(uint32_t date, double px)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = TimeUtils::timeToMinBar(date, 1500);
	bar.open = px;
	bar.high = px + 1.0;
	bar.low = px - 1.0;
	bar.close = px;
	bar.settle = px;
	bar.vol = 10;
	bar.hold = 100;
	return bar;
}

inline WTSBarStruct make_min_bar(uint32_t date, uint32_t hhmm, double px)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = TimeUtils::timeToMinBar(date, hhmm);
	bar.open = px;
	bar.high = px + 0.5;
	bar.low = px - 0.5;
	bar.close = px;
	bar.settle = px;
	bar.vol = 5;
	bar.hold = 50;
	return bar;
}

inline WTSTickStruct make_tick(const char* exchg, const char* code, uint32_t date,
	uint32_t actionTime, double px)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	snprintf(tick.exchg, sizeof(tick.exchg), "%s", exchg);
	snprintf(tick.code, sizeof(tick.code), "%s", code);
	tick.trading_date = date;
	tick.action_date = date;
	tick.action_time = actionTime;
	tick.price = px;
	tick.open = px;
	tick.high = px;
	tick.low = px;
	tick.settle_price = px;
	tick.pre_close = px;
	tick.pre_settle = px;
	tick.volume = 1;
	tick.total_volume = 1;
	tick.turn_over = px;
	tick.total_turnover = px;
	tick.open_interest = 1;
	return tick;
}

inline bool write_dsb(const fs::path& path, uint16_t type, const void* data, std::size_t bytes)
{
	fs::create_directories(path.parent_path());

	BlkHeader head;
	memset(&head, 0, sizeof(head));
	memcpy(head._blk_flag, "&^%$#@!", 8);
	head._type = type;
	head._version = 3;	// BLOCK_VERSION_RAW_V2

	std::ofstream ofs(path.string(), std::ios::binary | std::ios::trunc);
	if (!ofs)
		return false;

	ofs.write(reinterpret_cast<const char*>(&head), sizeof(head));
	if (bytes > 0)
		ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
	ofs.close();
	return ofs.good();
}

inline bool seed_day_bars(const std::string& storeWithSlash, const char* exchg,
	const char* code, const std::vector<WTSBarStruct>& bars)
{
	const fs::path p = fs::path(storeWithSlash) / "his" / "day" / exchg / (std::string(code) + ".dsb");
	return write_dsb(p, BT_HIS_Day, bars.data(), bars.size() * sizeof(WTSBarStruct));
}

inline bool seed_min_bars(const std::string& storeWithSlash, const char* pname,
	const char* exchg, const char* code, const std::vector<WTSBarStruct>& bars)
{
	const fs::path p = fs::path(storeWithSlash) / "his" / pname / exchg / (std::string(code) + ".dsb");
	const uint16_t type = (strcmp(pname, "min5") == 0) ? BT_HIS_Minute5 : BT_HIS_Minute1;
	return write_dsb(p, type, bars.data(), bars.size() * sizeof(WTSBarStruct));
}

inline bool seed_ticks(const std::string& storeWithSlash, const char* exchg,
	const char* code, uint32_t date, const std::vector<WTSTickStruct>& ticks)
{
	const fs::path p = fs::path(storeWithSlash) / "his" / "ticks" / exchg
		/ std::to_string(date) / (std::string(code) + ".dsb");
	return write_dsb(p, BT_HIS_Ticks, ticks.data(), ticks.size() * sizeof(WTSTickStruct));
}

/*!
 * 默认数据集：
 *  - 日线 20240102..20240105 共 4 根
 *  - 1 分钟 09:31..09:40 共 10 根
 *  - 5 分钟 09:35/09:40/09:45 共 3 根
 *  - IC2204 tick 09:30:00/09:30:30/09:31:00，600000 tick 09:30:00/09:31:00
 */
inline bool seed_default_store(const std::string& storeWithSlash)
{
	std::vector<WTSBarStruct> days;
	for (uint32_t i = 0; i < 4; i++)
		days.push_back(make_day_bar(20240102 + i, 100.0 + i));
	if (!seed_day_bars(storeWithSlash, "CFFEX", "IC2204", days))
		return false;

	std::vector<WTSBarStruct> mins1;
	for (uint32_t i = 0; i < 10; i++)
		mins1.push_back(make_min_bar(20240102, 931 + i, 50.0 + i));
	if (!seed_min_bars(storeWithSlash, "min1", "CFFEX", "IC2204", mins1))
		return false;

	std::vector<WTSBarStruct> mins5;
	mins5.push_back(make_min_bar(20240102, 935, 60.0));
	mins5.push_back(make_min_bar(20240102, 940, 61.0));
	mins5.push_back(make_min_bar(20240102, 945, 62.0));
	if (!seed_min_bars(storeWithSlash, "min5", "CFFEX", "IC2204", mins5))
		return false;

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick("CFFEX", "IC2204", 20240102, 93000000, 100.0));
	ticks.push_back(make_tick("CFFEX", "IC2204", 20240102, 93030000, 100.5));
	ticks.push_back(make_tick("CFFEX", "IC2204", 20240102, 93100000, 101.0));
	if (!seed_ticks(storeWithSlash, "CFFEX", "IC2204", 20240102, ticks))
		return false;

	std::vector<WTSTickStruct> stockTicks;
	stockTicks.push_back(make_tick("SSE", "600000", 20240102, 93000000, 12.0));
	stockTicks.push_back(make_tick("SSE", "600000", 20240102, 93100000, 12.5));
	if (!seed_ticks(storeWithSlash, "SSE", "600000", 20240102, stockTicks))
		return false;

	return true;
}

}	// namespace r102
