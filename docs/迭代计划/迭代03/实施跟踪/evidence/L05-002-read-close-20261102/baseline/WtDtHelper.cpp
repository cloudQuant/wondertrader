/*!
 * \file WtDtPorter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtDtHelper.h"
#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/Converter.hpp"
#include "../Share/fmtlib.h"
#include "../Share/StdUtils.hpp"

#include "../WtDataStorage/DataDefine.h"
#include "../WTSUtils/WTSCmpHelper.hpp"
#include "../WTSTools/CsvHelper.h"
#include "../WTSTools/WTSDataFactory.h"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

#include <cctype>
#include <exception>
#include <fstream>
#include <limits>

#include <rapidjson/document.h>
namespace rj = rapidjson;

USING_NS_WTP;

#ifdef WT_DT_HELPER_CLOSE_TEST_HOOKS
namespace
{
	thread_local bool g_fail_next_block_close_for_test = false;
}

extern "C" void wt_dt_helper_fail_next_close_for_test()
{
	g_fail_next_block_close_for_test = true;
}
#endif

/*
 *	处理块数据
 */
bool proc_block_data(std::string& content, bool isBar, bool bKeepHead /* = true */)
{
	if (content.size() < BLOCK_HEADER_SIZE)
		return false;

	BlockHeader* header = (BlockHeader*)content.data();

	bool bCmped = header->is_compressed();
	bool bOldVer = header->is_old_version();

	//如果既没有压缩，也不是老版本结构体，则直接返回
	if (!bCmped && !bOldVer)
	{
		if (!bKeepHead)
			content.erase(0, BLOCK_HEADER_SIZE);
		return true;
	}

	std::string buffer;
	if (bCmped)
	{
		// Compressed blocks use the extended header even when they contain an
		// old-version payload. Do not read _size until the complete header exists.
		if (content.size() < BLOCK_HEADERV2_SIZE)
			return false;

		BlockHeaderV2* blkV2 = (BlockHeaderV2*)content.c_str();

		const size_t compressedSize = content.size() - BLOCK_HEADERV2_SIZE;
		if (blkV2->_size != compressedSize)
		{
			return false;
		}

		// WTSCmpHelper::uncompress_data can throw for an invalid frame (and for
		// frames without a declared content size). Treat malformed files as a
		// decode failure instead of letting the exception escape the reader.
		const void* compressedData = content.data() + BLOCK_HEADERV2_SIZE;
		const unsigned long long uncompressedSize = ZSTD_getFrameContentSize(compressedData, compressedSize);
		std::string uncompressedBuffer;
		if (uncompressedSize == ZSTD_CONTENTSIZE_ERROR ||
			uncompressedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
			uncompressedSize > uncompressedBuffer.max_size())
			return false;
		if (isBar)
		{
			const size_t barSize = bOldVer ? sizeof(WTSBarStructOld) : sizeof(WTSBarStruct);
			if (uncompressedSize % barSize != 0)
				return false;
		}

		try
		{
			buffer = WTSCmpHelper::uncompress_data(compressedData, compressedSize);
		}
		catch (...)
		{
			return false;
		}
	}
	else
	{
		if (!bOldVer)
		{
			//如果不是老版本，直接返回
			if (!bKeepHead)
				content.erase(0, BLOCK_HEADER_SIZE);
			return true;
		}
		else
		{
			buffer.append(content.data() + BLOCK_HEADER_SIZE, content.size() - BLOCK_HEADER_SIZE);
		}
	}

	if (bOldVer)
	{
		if (isBar)
		{
			if (buffer.size() % sizeof(WTSBarStructOld) != 0)
				return false;

			std::string bufV2;
			uint32_t barcnt = buffer.size() / sizeof(WTSBarStructOld);
			bufV2.resize(barcnt * sizeof(WTSBarStruct));
			WTSBarStruct* newBar = (WTSBarStruct*)bufV2.data();
			WTSBarStructOld* oldBar = (WTSBarStructOld*)buffer.data();
			for (uint32_t idx = 0; idx < barcnt; idx++)
			{
				newBar[idx] = oldBar[idx];
			}
			buffer.swap(bufV2);
		}
		else
		{
			if (buffer.size() % sizeof(WTSTickStructOld) != 0)
				return false;

			uint32_t tick_cnt = buffer.size() / sizeof(WTSTickStructOld);
			std::string bufv2;
			bufv2.resize(sizeof(WTSTickStruct)*tick_cnt);
			WTSTickStruct* newTick = (WTSTickStruct*)bufv2.data();
			WTSTickStructOld* oldTick = (WTSTickStructOld*)buffer.data();
			for (uint32_t i = 0; i < tick_cnt; i++)
			{
				newTick[i] = oldTick[i];
			}
			buffer.swap(bufv2);
		}
	}
	else if (bCmped && isBar && buffer.size() % sizeof(WTSBarStruct) != 0)
	{
		return false;
	}

	if (bKeepHead)
	{
		content.resize(BLOCK_HEADER_SIZE);
		content.append(buffer);
		header = (BlockHeader*)content.data();
		header->_version = BLOCK_VERSION_RAW_V2;
	}
	else
	{
		content.swap(buffer);
	}

	return true;
}


uint32_t strToTime(const char* strTime, bool bKeepSec = false)
{
	std::string str;
	const char *pos = strTime;
	while (strlen(pos) > 0)
	{
		if (pos[0] != ':')
		{
			str.append(pos, 1);
		}
		pos++;
	}

	uint32_t ret = convert::to_uint32(str.c_str());
	if (ret > 10000 && !bKeepSec)
		ret /= 100;

	return ret;
}

bool parseUnsignedDatePart(const std::string& value, uint32_t& result)
{
	if (value.empty())
		return false;

	uint32_t parsed = 0;
	for (char ch : value)
	{
		if (!std::isdigit((unsigned char)ch))
			return false;
		parsed = parsed * 10 + (uint32_t)(ch - '0');
	}
	result = parsed;
	return true;
}

bool strToDate(const char* strDate, uint32_t& result)
{
	if (strDate == NULL)
		return false;

	std::string date(strDate);
	const size_t first = date.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return false;
	const size_t last = date.find_last_not_of(" \t\r\n");
	date = date.substr(first, last - first + 1);

	// CSV sources commonly place a time suffix in the date cell; only the date
	// portion participates in conversion, matching the historic behavior.
	const size_t timeStart = date.find_first_of(" \t");
	if (timeStart != std::string::npos)
		date.resize(timeStart);

	uint32_t year = 0, month = 0, day = 0;
	const size_t slash = date.find('/');
	const size_t hyphen = date.find('-');
	if (slash == std::string::npos && hyphen == std::string::npos)
	{
		if (date.size() != 8 || !parseUnsignedDatePart(date, result))
			return false;
		year = result / 10000;
		month = (result / 100) % 100;
		day = result % 100;
	}
	else
	{
		const char separator = slash != std::string::npos ? '/' : '-';
		std::vector<std::string> parts;
		size_t start = 0;
		while (true)
		{
			const size_t separatorAt = date.find(separator, start);
			parts.emplace_back(date.substr(start, separatorAt == std::string::npos ? std::string::npos : separatorAt - start));
			if (separatorAt == std::string::npos)
				break;
			start = separatorAt + 1;
		}

		if (parts.size() != 3 || parts[0].size() != 4 || parts[1].empty() || parts[1].size() > 2 ||
			parts[2].empty() || parts[2].size() > 2 ||
			!parseUnsignedDatePart(parts[0], year) || !parseUnsignedDatePart(parts[1], month) ||
			!parseUnsignedDatePart(parts[2], day))
			return false;
	}

	if (year == 0 || month == 0 || month > 12)
		return false;
	static const uint8_t daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	uint32_t maxDay = daysInMonth[month - 1];
	if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
		maxDay = 29;
	if (day == 0 || day > maxDay)
		return false;

	result = year * 10000 + month * 100 + day;
	return true;
}

bool readHelperFile(const std::string& path, std::string& content, FuncLogCallback cbLogger)
{
	try
	{
		StdFile::read_file_content(path.c_str(), content);
		return true;
	}
	catch (const std::exception& e)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"读取数据文件{}失败: {}", path.c_str(), e.what()));
	}
	catch (...)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"读取数据文件{}失败: 未知异常", path.c_str()));
	}
	return false;
}

namespace
{
	bool write_csv_file(WtString filename, const std::string& content, FuncLogCallback cbLogger);
}

void dump_bars(WtString binFolder, WtString csvFolder, WtString /*strFilter*/ /* = "" */, FuncLogCallback cbLogger /* = NULL */)
{
	std::string srcFolder = StrUtil::standardisePath(binFolder);
	if (!StdFile::exists(srcFolder.c_str()))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"目录{}不存在", binFolder));
		return;
	}

	boost::system::error_code ec;
	if (!StdFile::exists(csvFolder))
		fs::create_directories(csvFolder, ec);
	if (ec || !fs::is_directory(csvFolder, ec))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"目录{}创建失败", csvFolder));
		return;
	}
	bool allSucceeded = true;

	fs::path myPath(srcFolder);
	fs::directory_iterator endIter;
	fs::directory_iterator iter(myPath, ec);
	for (; !ec && iter != endIter; iter.increment(ec))
	{
		const bool isDir = fs::is_directory(iter->path(), ec);
		if (ec)
			break;
		if (isDir)
			continue;

		if (iter->path().extension() != ".dsb")
			continue;

		const std::string& path = iter->path().string();

		std::string fileCode = iter->path().stem().string();

		if (cbLogger)
			cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path));

		std::string buffer;
		if (!readHelperFile(path, buffer, cbLogger))
		{
			allSucceeded = false;
			continue;
		}
		if (buffer.size() < sizeof(HisKlineBlock))
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"文件{}头部校验失败", path.c_str()));
			allSucceeded = false;
			continue;
		}

		BlockHeader* bHeader = (BlockHeader*)buffer.data();

		if(bHeader->_type < BT_HIS_Minute1 || bHeader->_type > BT_HIS_Day)
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"文件{}不是K线数据，跳过转换", path.c_str()));
			continue;
		}

		bool isDay = (bHeader->_type == BT_HIS_Day);

		if (!proc_block_data(buffer, true, false) || buffer.size() % sizeof(WTSBarStruct) != 0)
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"文件{}数据块校验失败，跳过转换", path.c_str()));
			allSucceeded = false;
			continue;
		}

		auto kcnt = buffer.size() / sizeof(WTSBarStruct);
		if (kcnt <= 0)
			continue;

		std::string filename = StrUtil::standardisePath(csvFolder);
		filename += fileCode;
		filename += ".csv";

		if (cbLogger)
			cbLogger(fmtutil::format(u8"正在写入{}...", filename));

		WTSBarStruct* bars = (WTSBarStruct*)buffer.data();

		std::stringstream ss;
		ss << "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest" << std::endl;
		ss.setf(std::ios::fixed);

		for (uint32_t i = 0; i < kcnt; i++)
		{
			const WTSBarStruct& curBar = bars[i];
			if(isDay)
			{
				ss << curBar.date << ",0,";
			}
			else
			{
				uint32_t barTime = (uint32_t)(curBar.time % 10000 * 100);
				uint32_t barDate = (uint32_t)(curBar.time / 10000 + 19900000);
				ss << barDate << ","
					<< barTime << ",";
			}
			
			ss << curBar.open << ","
				<< curBar.high << ","
				<< curBar.low << ","
				<< curBar.close << ","
				<< curBar.settle << ","
				<< curBar.vol << ","
				<< curBar.money << ","
				<< curBar.hold << ","
				<< curBar.add << std::endl;
		}

		if (!write_csv_file(filename.c_str(), ss.str(), cbLogger))
		{
			allSucceeded = false;
			continue;
		}

		if (cbLogger)
			cbLogger(fmtutil::format(u8"{}写入完成,共{}条bar", filename, kcnt));
	}

	if (ec)
	{
		allSucceeded = false;
		if (cbLogger)
			cbLogger(fmtutil::format(u8"目录{}读取失败: {}", binFolder, ec.message()));
	}
	if (cbLogger && allSucceeded)
		cbLogger(fmtutil::format(u8"目录{}全部导出完成...", binFolder));
}

void dump_ticks(WtString binFolder, WtString csvFolder, WtString /*strFilter*/ /* = "" */, FuncLogCallback cbLogger /* = NULL */)
{
	std::string srcFolder = StrUtil::standardisePath(binFolder);
	if (!StdFile::exists(srcFolder.c_str()))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"目录{}不存在", binFolder));
		return;
	}

	boost::system::error_code ec;
	if (!StdFile::exists(csvFolder))
		fs::create_directories(csvFolder, ec);
	if (ec || !fs::is_directory(csvFolder, ec))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"目录{}创建失败", csvFolder));
		return;
	}
	bool allSucceeded = true;

	fs::path myPath(srcFolder);
	fs::directory_iterator endIter;
	fs::directory_iterator iter(myPath, ec);
	for (; !ec && iter != endIter; iter.increment(ec))
	{
		const bool isDir = fs::is_directory(iter->path(), ec);
		if (ec)
			break;
		if (isDir)
			continue;

		if (iter->path().extension() != ".dsb")
			continue;

		const std::string& path = iter->path().string();

		std::string fileCode = iter->path().stem().string();

		if (cbLogger)
			cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

		std::string buffer;
		if (!readHelperFile(path, buffer, cbLogger))
		{
			allSucceeded = false;
			continue;
		}
		if (buffer.size() < sizeof(HisTickBlock))
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"文件{}头部校验失败", path.c_str()));
			allSucceeded = false;
			continue;
		}

		if (!proc_block_data(buffer, false, false) || buffer.size() % sizeof(WTSTickStruct) != 0)
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"文件{}数据块校验失败，跳过转换", path.c_str()));
			allSucceeded = false;
			continue;
		}

		auto tcnt = buffer.size() / sizeof(WTSTickStruct);
		if (tcnt <= 0)
			continue;

		std::string filename = StrUtil::standardisePath(csvFolder);
		filename += fileCode;
		filename += ".csv";

		if (cbLogger)
			cbLogger(fmtutil::format(u8"正在写入{}...", filename.c_str()));

		WTSTickStruct* ticks = (WTSTickStruct*)buffer.data();

		std::stringstream ss;
		ss.setf(std::ios::fixed, std::ios::floatfield);
		ss.precision(6);
		ss << "exchg,code,tradingdate,actiondate,actiontime,price,open,high,low,settle,preclose,"
			<< "presettle,preinterest,total_volume,total_turnover,open_interest,volume,turnover,additional,";
		for (int i = 0; i < 10; i++)
		{
			bool hasTail = (i != 9);
			ss << "bidprice" << i + 1 << "," << "bidqty" << i + 1 << "," << "askprice" << i + 1 << "," << "askqty" << i + 1 << (hasTail ? "," : "");
		}
		ss << std::endl;

		for (uint32_t i = 0; i < tcnt; i++)
		{
			const WTSTickStruct& curTick = ticks[i];
			ss << curTick.exchg << "," << curTick.code << ","
				<< curTick.trading_date << ","
				<< curTick.action_date << ","
				<< curTick.action_time << ","
				<< curTick.price << ","
				<< curTick.open << ","
				<< curTick.high << ","
				<< curTick.low << ","
				<< curTick.settle_price << ","
				<< curTick.pre_close << ","
				<< curTick.pre_settle << ","
				<< curTick.pre_interest << ","
				<< curTick.total_volume << ","
				<< curTick.total_turnover << ","
				<< curTick.open_interest << ","
				<< curTick.volume << ","
				<< curTick.turn_over << ","
				<< curTick.diff_interest << ",";

			for (int j = 0; j < 10; j++)
			{
				bool hasTail = (j != 9);
				ss << curTick.bid_prices[j] << "," << curTick.bid_qty[j] << "," << curTick.ask_prices[j] << "," << curTick.ask_qty[j] << (hasTail ? "," : "");
			}
			ss << std::endl;
		}

		if (!write_csv_file(filename.c_str(), ss.str(), cbLogger))
		{
			allSucceeded = false;
			continue;
		}

		if (cbLogger)
			cbLogger(fmtutil::format(u8"{}写入完成,共{}条tick数据", filename.c_str(), tcnt));
	}

	if (ec)
	{
		allSucceeded = false;
		if (cbLogger)
			cbLogger(fmtutil::format(u8"目录{}读取失败: {}", binFolder, ec.message()));
	}
	if (cbLogger && allSucceeded)
		cbLogger(fmtutil::format(u8"目录{}全部导出完成...", binFolder));
}

namespace
{
	void log_path_message(FuncLogCallback cbLogger, const char* prefix,
		WtString filename, const char* suffix)
	{
		if (!cbLogger)
			return;

		std::string message;
		try
		{
			message = prefix;
			message += filename == nullptr ? "" : filename;
			message += suffix;
		}
		catch (...)
		{
			cbLogger(u8"数据文件操作日志生成失败");
			return;
		}
		cbLogger(message.c_str());
	}

	bool write_csv_file(WtString filename, const std::string& content, FuncLogCallback cbLogger)
	{
		std::ofstream file(filename, std::ios::binary);
		if (!file.is_open())
		{
			log_path_message(cbLogger, u8"数据文件", filename, u8"创建失败");
			return false;
		}
		if (content.size() > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)()))
		{
			log_path_message(cbLogger, u8"数据文件", filename, u8"写入失败");
			return false;
		}
		file.write(content.data(), static_cast<std::streamsize>(content.size()));
		const bool written = !file.fail();
		file.close();
		bool closed = !file.fail();
#ifdef WT_DT_HELPER_CLOSE_TEST_HOOKS
		if (g_fail_next_block_close_for_test)
		{
			g_fail_next_block_close_for_test = false;
			closed = false;
		}
#endif
		if (!written)
		{
			log_path_message(cbLogger, u8"数据文件", filename, u8"写入失败");
			return false;
		}
		if (!closed)
		{
			log_path_message(cbLogger, u8"数据文件", filename, u8"关闭失败");
			return false;
		}
		return true;
	}

	bool write_block_file(WtString filename, const std::string& content,
		FuncLogCallback cbLogger, const char* successMessage, bool successIncludesPath = false)
	{
		BoostFile bf;
		if (!bf.create_new_file(filename))
		{
			bf.close_file();
			log_path_message(cbLogger, u8"数据文件", filename, u8"创建失败");
			return false;
		}

		if (!bf.write_file(content))
		{
			bf.close_file();
			log_path_message(cbLogger, u8"数据文件", filename, u8"写入失败");
			return false;
		}

		bool closed = bf.close_file_checked();
#ifdef WT_DT_HELPER_CLOSE_TEST_HOOKS
		// Test-only injection happens after the real close, so the handle is not
		// leaked; it tests propagation rather than a real operating-system fault.
		if (g_fail_next_block_close_for_test)
		{
			g_fail_next_block_close_for_test = false;
			closed = false;
		}
#endif
		if (!closed)
		{
			log_path_message(cbLogger, u8"数据文件", filename, u8"关闭失败");
			return false;
		}
		if (cbLogger)
		{
			if (successIncludesPath)
				log_path_message(cbLogger, successMessage, filename, "");
			else
				cbLogger(successMessage);
		}
		return true;
	}
}

void trans_csv_bars(WtString csvFolder, WtString binFolder, WtString period, FuncLogCallback cbLogger /* = NULL */)
{
	if (!StdFile::exists(csvFolder))
		return;

	if (!StdFile::exists(binFolder))
	{
		try
		{
			fs::create_directories(binFolder);
		}
		catch (const fs::filesystem_error& e)
		{
			if (cbLogger)
			{
				std::string message;
				try
				{
					message = fmt::format(u8"创建输出目录{}失败: {}", binFolder, e.what());
				}
				catch (...)
				{
					cbLogger(u8"创建输出目录失败");
					return;
				}
				cbLogger(message.c_str());
			}
			return;
		}
	}

	WTSKlinePeriod kp = KP_DAY;
	if (wt_stricmp(period, "m1") == 0)
		kp = KP_Minute1;
	else if (wt_stricmp(period, "m5") == 0)
		kp = KP_Minute5;
	else
		kp = KP_DAY;

	fs::path myPath(csvFolder);
	fs::directory_iterator endIter;
	for (fs::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		if (fs::is_directory(iter->path()))
			continue;

		if (iter->path().extension() != ".csv")
			continue;

		const std::string& path = iter->path().string();

		if(cbLogger)
			cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

		CsvReader reader(",");
		if(!reader.load_from_file(path.c_str()))
		{
			if (cbLogger)
				cbLogger(fmtutil::format(u8"读取数据文件{}失败...", path.c_str()));
			continue;
		}

		std::vector<WTSBarStruct> bars;
		uint32_t lineNumber = 1; // CSV header occupies the first line.

		while(reader.next_row())
		{
			++lineNumber;
			//逐行读取
			WTSBarStruct bs;
			if (!strToDate(reader.get_string("date"), bs.date))
			{
				if (cbLogger)
					cbLogger(fmtutil::format(u8"文件{}第{}行日期无效，已跳过", path.c_str(), lineNumber));
				continue;
			}
			if(kp != KP_DAY)
				bs.time = TimeUtils::timeToMinBar(bs.date, strToTime(reader.get_string("time")));
			bs.open = reader.get_double("open");
			bs.high = reader.get_double("high");
			bs.low = reader.get_double("low");
			bs.close = reader.get_double("close");
			bs.vol = reader.get_double("volume");
			bs.money = reader.get_double("turnover");
			bs.hold = reader.get_double("open_interest");
			bs.add = reader.get_double("diff_interest");
			bs.settle = reader.get_double("settle");
			bars.emplace_back(bs);

			if (bars.size() % 1000 == 0)
			{
				if (cbLogger)
					cbLogger(fmtutil::format(u8"已读取数据{}条", bars.size()));
			}
		}
		if (cbLogger)
			cbLogger(fmtutil::format(u8"数据文件{}全部读取完成,共{}条", path.c_str(), bars.size()));

		BlockType btype;
		switch (kp)
		{
		case KP_Minute1: btype = BT_HIS_Minute1; break;
		case KP_Minute5: btype = BT_HIS_Minute5; break;
		default: btype = BT_HIS_Day; break;
		}

		HisKlineBlockV2 kBlock;
		strcpy(kBlock._blk_flag, BLK_FLAG);
		kBlock._type = btype;
		kBlock._version = BLOCK_VERSION_CMP_V2;

		std::string cmprsData = WTSCmpHelper::compress_data(bars.data(), sizeof(WTSBarStruct)*bars.size());
		kBlock._size = cmprsData.size();

		std::string filename = StrUtil::standardisePath(binFolder);
		filename += iter->path().stem().string();
		filename += ".dsb";

		std::string content;
		content.append((const char*)&kBlock, sizeof(HisKlineBlockV2));
		content.append(cmprsData);
		write_block_file(filename.c_str(), content, cbLogger, u8"数据已转储至", true);
	}
}

//bool trans_bars(WtString barFile, FuncGetBarItem getter, int count, WtString period, FuncLogCallback cbLogger /* = NULL */)
//{
//	if (count == 0)
//	{
//		if (cbLogger)
//			cbLogger("K线数据条数为0");
//		return false;
//	}
//
//	BlockType bType = BT_HIS_Day;
//	if (wt_stricmp(period, "m1") == 0)
//		bType = BT_HIS_Minute1;
//	else if (wt_stricmp(period, "m5") == 0)
//		bType = BT_HIS_Minute5;
//	else if(wt_stricmp(period, "d") == 0)
//		bType = BT_HIS_Day;
//	else
//	{
//		if (cbLogger)
//			cbLogger("周期只能为m1、m5或d");
//		return false;
//	}
//
//	std::string buffer;
//	buffer.resize(sizeof(WTSBarStruct)*count);
//	WTSBarStruct* bars = (WTSBarStruct*)buffer.c_str();
//	int realCnt = 0;
//	for(int i = 0; i < count; i++)
//	{
//		bool bSucc = getter(&bars[i], i);
//		if (!bSucc)
//			break;
//
//		realCnt++;
//	}
//
//	if (realCnt != count)
//	{
//		buffer.resize(sizeof(WTSBarStruct)*realCnt);
//	}
//
//	if (cbLogger)
//		cbLogger("K线数据已经读取完成，准备写入文件");
//
//	std::string content;
//	content.resize(sizeof(HisKlineBlockV2));
//	HisKlineBlockV2* block = (HisKlineBlockV2*)content.data();
//	strcpy(block->_blk_flag, BLK_FLAG);
//	block->_version = BLOCK_VERSION_CMP;
//	block->_type = bType;
//	std::string cmp_data = WTSCmpHelper::compress_data(bars, buffer.size());
//	block->_size = cmp_data.size();
//	content.append(cmp_data);
//
//	BoostFile bf;
//	if (bf.create_new_file(barFile))
//	{
//		bf.write_file(content);
//	}
//	bf.close_file();
//
//	if (cbLogger)
//		cbLogger("K线数据写入文件成功");
//	return true;
//}
//
//bool trans_ticks(WtString tickFile, FuncGetTickItem getter, int count, FuncLogCallback cbLogger/* = NULL*/)
//{
//	if (count == 0)
//	{
//		if (cbLogger)
//			cbLogger("Tick数据条数为0");
//		return false;
//	}
//
//	std::string buffer;
//	buffer.resize(sizeof(WTSTickStruct)*count);
//	WTSTickStruct* ticks = (WTSTickStruct*)buffer.c_str();
//	int realCnt = 0;
//	for (int i = 0; i < count; i++)
//	{
//		bool bSucc = getter(&ticks[i], i);
//		if (!bSucc)
//			break;
//
//		realCnt++;
//	}
//
//	if(realCnt != count)
//	{
//		buffer.resize(sizeof(WTSTickStruct)*realCnt);
//	}
//
//	if (cbLogger)
//		cbLogger("Tick数据已经读取完成，准备写入文件");
//
//	std::string content;
//	content.resize(sizeof(HisKlineBlockV2));
//	HisKlineBlockV2* block = (HisKlineBlockV2*)content.data();
//	strcpy(block->_blk_flag, BLK_FLAG);
//	block->_version = BLOCK_VERSION_CMP;
//	block->_type = BT_HIS_Ticks;
//	std::string cmp_data = WTSCmpHelper::compress_data(ticks, buffer.size());
//	block->_size = cmp_data.size();
//	content.append(cmp_data);
//
//	BoostFile bf;
//	if (bf.create_new_file(tickFile))
//	{
//		bf.write_file(content);
//	}
//	bf.close_file();
//
//	if (cbLogger)
//		cbLogger("Tick数据写入文件成功");
//
//	return true;
//}

WtUInt32 read_dsb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger /* = NULL */)
{
	std::string path = tickFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	if (!readHelperFile(path, content, cbLogger))
		return 0;
	if (content.size() < sizeof(HisTickBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", tickFile));
		return 0;
	}

	if (!proc_block_data(content, false, false) || content.size() % sizeof(WTSTickStruct) != 0)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}数据块校验失败", tickFile));
		return 0;
	}

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSTickStruct);

	cbCnt(tcnt);
	cb((WTSTickStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条tick数据", tickFile, tcnt));

	return (WtUInt32)tcnt;
}

WtUInt32 read_dsb_order_details(WtString dataFile, FuncGetOrdDtlCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger/* = NULL*/)
{
	std::string path = dataFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	if (!readHelperFile(path, content, cbLogger))
		return 0;
	if (content.size() < sizeof(HisOrdDtlBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", dataFile));
		return 0;
	}

	if (!proc_block_data(content, false, false) || content.size() % sizeof(WTSOrdDtlStruct) != 0)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}数据块校验失败", dataFile));
		return 0;
	}

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSOrdDtlStruct);

	cbCnt(tcnt);
	cb((WTSOrdDtlStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条order detail数据", dataFile, tcnt));

	return (WtUInt32)tcnt;
}

WtUInt32 read_dsb_order_queues(WtString dataFile, FuncGetOrdQueCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger/* = NULL*/)
{
	std::string path = dataFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	if (!readHelperFile(path, content, cbLogger))
		return 0;
	if (content.size() < sizeof(HisOrdQueBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", dataFile));
		return 0;
	}

	if (!proc_block_data(content, false, false) || content.size() % sizeof(WTSOrdQueStruct) != 0)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}数据块校验失败", dataFile));
		return 0;
	}

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSOrdQueStruct);

	cbCnt(tcnt);
	cb((WTSOrdQueStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条order queue数据", dataFile, tcnt));

	return (WtUInt32)tcnt;
}

WtUInt32 read_dsb_transactions(WtString dataFile, FuncGetTransCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger/* = NULL*/)
{
	std::string path = dataFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	if (!readHelperFile(path, content, cbLogger))
		return 0;
	if (content.size() < sizeof(HisTransBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", dataFile));
		return 0;
	}

	if (!proc_block_data(content, false, false) || content.size() % sizeof(WTSTransStruct) != 0)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}数据块校验失败", dataFile));
		return 0;
	}

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSTransStruct);

	cbCnt(tcnt);
	cb((WTSTransStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条transaction数据", dataFile, tcnt));

	return (WtUInt32)tcnt;
}

WtUInt32 read_dsb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger )
{
	std::string path = barFile;
	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string content;
	if (!readHelperFile(path, content, cbLogger))
		return 0;
	if (content.size() < sizeof(HisKlineBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", barFile));
		return 0;
	}

	if (!proc_block_data(content, true, false) || content.size() % sizeof(WTSBarStruct) != 0)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}数据块校验失败", barFile));
		return 0;
	}

	if(content.empty())
	{
		cbCnt(0);
		return 0;
	}


	auto kcnt = content.size() / sizeof(WTSBarStruct);
	cbCnt(kcnt);
	cb((WTSBarStruct*)content.data(), kcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条bar", barFile, kcnt));

	return (WtUInt32)kcnt;
}

WtUInt32 read_dmb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger)
{
	std::string path = barFile;

	std::string buffer;
	if (!readHelperFile(path, buffer, cbLogger))
		return 0;
	if (buffer.size() < sizeof(RTKlineBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", barFile));
		return 0;
	}

	RTKlineBlock* tBlock = (RTKlineBlock*)buffer.c_str();
	auto kcnt = tBlock->_size;
	if (kcnt <= 0)
	{
		cbCnt(0);
		return 0;
	}

	cbCnt(kcnt);
	cb(tBlock->_bars, kcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条bar", barFile, kcnt));

	return (WtUInt32)kcnt;
}

WtUInt32 read_dmb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger /* = NULL */)
{
	std::string path = tickFile;

	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string buffer;
	StdFile::read_file_content(path.c_str(), buffer);
	if (buffer.size() < sizeof(RTTickBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", tickFile));
		return 0;
	}

	RTTickBlock* tBlock = (RTTickBlock*)buffer.c_str();
	auto tcnt = tBlock->_size;
	if (tcnt <= 0)
	{
		cbCnt(0);
		return 0;
	}

	cbCnt(tcnt);
	cb(tBlock->_ticks, tcnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}读取完成,共{}条tick数据", tickFile, tcnt));

	return (WtUInt32)tcnt;
}


WtUInt32 resample_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, WtUInt64 fromTime, WtUInt64 endTime,
	WtString period, WtUInt32 times, WtString sessInfo, FuncLogCallback cbLogger /* = NULL */, bool bAlignSec/* = false*/)
{
	WTSKlinePeriod kp;
	if(wt_stricmp(period, "m1") == 0)
	{
		kp = KP_Minute1;
	}
	else if (wt_stricmp(period, "m5") == 0)
	{
		kp = KP_Minute5;
	}
	else if (wt_stricmp(period, "d") == 0)
	{
		kp = KP_DAY;
	}
	else if (period[0] == 'h')
	{
		kp = KP_Minute5;
		times = PERIOD_TIMES_HOUR;
	}
	else if (period[0] == 'f')
	{
		kp = KP_Minute5;
		times = PERIOD_TIMES_HALF;
	}
	else
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"周期{}不是基础周期...", period));
		return 0;
	}

	bool isDay = (kp == KP_DAY);

	if(isDay)
	{
		if(fromTime >= 100000000 || endTime > 100000000)
		{
			if (cbLogger)
				cbLogger(u8"日线基础数据的开始时间结束时间应为日期，格式如yyyymmdd");
			return 0;
		}
	}
	else
	{
		if (fromTime < 100000000 || endTime < 100000000)
		{
			if (cbLogger)
				cbLogger(u8"分钟线基础数据的开始时间结束时间应为时间，格式如yyyymmddHHMM");
			return 0;
		}
	}

	if(fromTime > endTime)
	{
		std::swap(fromTime, endTime);
	}

	WTSSessionInfo* sInfo = NULL;
	{
		rj::Document root;
		if (root.Parse(sessInfo).HasParseError())
		{
			if (cbLogger)
				cbLogger(u8"交易时间模板解析失败");
			return 0;
		}

		if (!root.IsObject() || !root.HasMember("offset") || !root["offset"].IsInt() ||
			!root.HasMember("sections") || !root["sections"].IsArray())
		{
			if (cbLogger)
				cbLogger(u8"交易时间模板格式错误");
			return 0;
		}

		int32_t offset = root["offset"].GetInt();
		bool hasAuction = false;
		uint32_t auctionFrom = 0;
		uint32_t auctionTo = 0;
		if (root.HasMember("auction") && !root["auction"].IsNull())
		{
			const rj::Value& jAuc = root["auction"];
			if (!jAuc.IsObject() || !jAuc.HasMember("from") || !jAuc["from"].IsUint() ||
				!jAuc.HasMember("to") || !jAuc["to"].IsUint())
			{
				if (cbLogger)
					cbLogger(u8"交易时间模板格式错误");
				return 0;
			}

			hasAuction = true;
			auctionFrom = jAuc["from"].GetUint();
			auctionTo = jAuc["to"].GetUint();
		}

		const rj::Value& jSecs = root["sections"];
		for (const rj::Value& jSec : jSecs.GetArray())
		{
			if (!jSec.IsObject() || !jSec.HasMember("from") || !jSec["from"].IsUint() ||
				!jSec.HasMember("to") || !jSec["to"].IsUint())
			{
				if (cbLogger)
					cbLogger(u8"交易时间模板格式错误");
				return 0;
			}
		}

		// Create the session only after the complete JSON structure is validated,
		// so malformed input cannot leak a partially initialized session object.
		sInfo = WTSSessionInfo::create("tmp", "tmp", offset);
		if (hasAuction)
			sInfo->setAuctionTime(auctionFrom, auctionTo);
		for (const rj::Value& jSec : jSecs.GetArray())
			sInfo->addTradingSection(jSec["from"].GetUint(), jSec["to"].GetUint());
	}

	std::string path = barFile;
	if (cbLogger)
		cbLogger(fmtutil::format(u8"正在读取数据文件{}...", path.c_str()));

	std::string buffer;
	if (!readHelperFile(path, buffer, cbLogger))
	{
		sInfo->release();
		return 0;
	}
	if (buffer.size() < sizeof(HisKlineBlock))
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}头部校验失败", barFile));
		sInfo->release();
		return 0;
	}

	if (!proc_block_data(buffer, true, false) || buffer.size() % sizeof(WTSBarStruct) != 0)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"文件{}数据块校验失败", barFile));
		sInfo->release();
		return 0;
	}

	auto kcnt = buffer.size() / sizeof(WTSBarStruct);
	if (kcnt <= 0)
	{
		if (cbLogger)
			cbLogger(fmtutil::format(u8"{}数据为空", barFile));
		return 0;
	}

	WTSBarStruct* bars = (WTSBarStruct*)buffer.c_str();

	//确定第一条K线的位置
	WTSBarStruct bar;
	if (isDay)
		bar.date = (uint32_t)fromTime;
	else
	{

		bar.time = fromTime % 100000000 + ((fromTime / 100000000) - 1990) * 100000000;
	}

	WTSBarStruct* pBar = std::lower_bound(bars, bars + (kcnt - 1), bar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});


	uint32_t sIdx = (uint32_t)(pBar - bars);
	if((isDay && pBar->date < bar.date) || (!isDay && pBar->time < bar.time))
	{
		//如果返回的K线的时间小于要查找的时间，说明没有符合条件的数据
		if (cbLogger)
			cbLogger(u8"没有找到指定时间范围的K线");
		return 0;
	}
	else if (sIdx != 0 && ((isDay && pBar->date > bar.date) || (!isDay && pBar->time > bar.time)))
	{
		pBar--;
		sIdx--;
	}

	//确定最后一条K线的位置
	if (isDay)
		bar.date = (uint32_t)endTime;
	else
	{

		bar.time = endTime % 100000000 + ((endTime / 100000000) - 1990) * 100000000;
	}
	pBar = std::lower_bound(bars, bars + (kcnt - 1), bar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});

	uint32_t eIdx = 0;
	if (pBar == NULL)
		eIdx = kcnt - 1;
	else
		eIdx = (uint32_t)(pBar - bars);

	if (eIdx != 0 && ((isDay && pBar->date > bar.date) || (!isDay && pBar->time > bar.time)))
	{
		pBar--;
		eIdx--;
	}

	uint32_t hitCnt = eIdx - sIdx + 1;
	WTSKlineSlice* slice = WTSKlineSlice::create("", kp, 1, &bars[sIdx], hitCnt);
	WTSDataFactory fact;
	WTSKlineData* kline = fact.extractKlineData(slice, kp, times, sInfo, true, bAlignSec);
	if(kline == NULL)
	{
		if (cbLogger)
			cbLogger(u8"K线重采样失败");
		return 0;
	}

	uint32_t newCnt = kline->size();
	cbCnt(newCnt);
	cb(&kline->getDataRef().at(0),newCnt, true);

	if (cbLogger)
		cbLogger(fmtutil::format(u8"{}重采样完成,共将{}条bar重采样为{}条新bar", barFile, hitCnt, newCnt));

	
	kline->release();
	sInfo->release();
	slice->release();

	return (WtUInt32)newCnt;
}

bool store_bars(WtString barFile, WTSBarStruct* firstBar, int count, WtString period, FuncLogCallback cbLogger /* = NULL */)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger(u8"K线数据条数为0");
		return false;
	}

	BlockType bType = BT_HIS_Day;
	if (wt_stricmp(period, "m1") == 0)
		bType = BT_HIS_Minute1;
	else if (wt_stricmp(period, "m5") == 0)
		bType = BT_HIS_Minute5;
	else if (wt_stricmp(period, "d") == 0)
		bType = BT_HIS_Day;
	else
	{
		if (cbLogger)
			cbLogger(u8"周期只能为m1、m5或d");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSBarStruct)*count);
	WTSBarStruct* bars = (WTSBarStruct*)buffer.c_str();
	memcpy(bars, firstBar, sizeof(WTSBarStruct)*count);

	if (cbLogger)
		cbLogger(u8"K线数据已经读取完成，准备写入文件");

	std::string content;
	content.resize(sizeof(HisKlineBlockV2));
	HisKlineBlockV2* block = (HisKlineBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = bType;
	std::string cmp_data = WTSCmpHelper::compress_data(bars, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	return write_block_file(barFile, content, cbLogger, u8"K线数据写入文件成功");
}

bool store_ticks(WtString tickFile, WTSTickStruct* firstTick, int count, FuncLogCallback cbLogger/* = NULL*/)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger(u8"Tick数据条数为0");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSTickStruct)*count);
	WTSTickStruct* ticks = (WTSTickStruct*)buffer.c_str();
	memcpy(ticks, firstTick, sizeof(WTSTickStruct)*count);

	if (cbLogger)
		cbLogger(u8"Tick数据已经读取完成，准备写入文件");

	std::string content;
	content.resize(sizeof(HisTickBlockV2));
	HisTickBlockV2* block = (HisTickBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = BT_HIS_Ticks;
	std::string cmp_data = WTSCmpHelper::compress_data(ticks, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	return write_block_file(tickFile, content, cbLogger, u8"Tick数据写入文件成功");
}

bool store_order_details(WtString tickFile, WTSOrdDtlStruct* firstItem, int count, FuncLogCallback cbLogger/* = NULL*/)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger("Size of OrderDetail is 0");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSOrdDtlStruct)*count);
	WTSOrdDtlStruct* items = (WTSOrdDtlStruct*)buffer.c_str();
	memcpy(items, firstItem, sizeof(WTSOrdDtlStruct)*count);

	if (cbLogger)
		cbLogger("Reading order details done, prepare to write...");

	std::string content;
	content.resize(sizeof(HisOrdDtlBlockV2));
	HisOrdDtlBlockV2* block = (HisOrdDtlBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = BT_HIS_OrdDetail;
	std::string cmp_data = WTSCmpHelper::compress_data(items, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	return write_block_file(tickFile, content, cbLogger, "Writing order details succeed");
}

bool store_order_queues(WtString tickFile, WTSOrdQueStruct* firstItem, int count, FuncLogCallback cbLogger/* = NULL*/)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger("Size of order queues is 0");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSOrdQueStruct)*count);
	WTSOrdQueStruct* items = (WTSOrdQueStruct*)buffer.c_str();
	memcpy(items, firstItem, sizeof(WTSOrdQueStruct)*count);

	if (cbLogger)
		cbLogger("Reading order queues done, prepare to write...");

	std::string content;
	content.resize(sizeof(HisOrdQueBlockV2));
	HisOrdQueBlockV2* block = (HisOrdQueBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = BT_HIS_OrdQueue;
	std::string cmp_data = WTSCmpHelper::compress_data(items, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	return write_block_file(tickFile, content, cbLogger, "Writing order queues to file succeedd");
}

bool store_transactions(WtString tickFile, WTSTransStruct* firstItem, int count, FuncLogCallback cbLogger/* = NULL*/)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger("Size of transations is 0");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSTransStruct)*count);
	WTSTransStruct* items = (WTSTransStruct*)buffer.c_str();
	memcpy(items, firstItem, sizeof(WTSTransStruct)*count);

	if (cbLogger)
		cbLogger("Reading transactions done, prepare to write...");

	std::string content;
	content.resize(sizeof(HisTransBlockV2));
	HisTransBlockV2* block = (HisTransBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = BT_HIS_Trnsctn;
	std::string cmp_data = WTSCmpHelper::compress_data(items, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	return write_block_file(tickFile, content, cbLogger, "Write transactions to file succeedd");
}
