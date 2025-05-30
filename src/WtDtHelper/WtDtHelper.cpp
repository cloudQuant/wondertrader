/**
 * @file WtDtHelper.cpp
 * @project WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief WonderTrader数据处理辅助工具实现文件
 * 
 * @details 该文件实现了WonderTrader数据处理辅助工具的接口函数，
 * 包括数据转换、数据导出、数据读取和数据重采样等功能。
 * 这些函数主要用于处理K线、Tick、委托明细、委托队列和成交数据，
 * 支持二进制数据与CSV格式之间的转换，以及数据的读取和重采样操作。
 */
#include "WtDtHelper.h"
#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/BoostFile.hpp"

#include "../WtDataStorage/DataDefine.h"
#include "../WTSUtils/WTSCmpHelper.hpp"
#include "../WTSTools/CsvHelper.h"
#include "../WTSTools/WTSDataFactory.h"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"

#include <rapidjson/document.h>

namespace rj = rapidjson;

USING_NS_WTP;

/**
 * @brief 处理数据块内容
 * @param content 数据块内容，传入传出参数
 * @param isBar 是否是K线数据块，如果是false则表示是Tick数据块
 * @param bKeepHead 是否保留块头部，默认为true
 * @return bool 处理是否成功
 * 
 * @details 该函数用于处理数据块内容，包括解压缩、版本转换等操作。
 * 如果数据块是压缩的，则先解压缩；如果是旧版本的数据结构，则转换为新版本。
 * 该函数主要用于数据读取和转换过程中的数据预处理。
 */
bool proc_block_data(std::string& content, bool isBar, bool bKeepHead /* = true */)
{
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
		BlockHeaderV2* blkV2 = (BlockHeaderV2*)content.c_str();

		if (content.size() != (sizeof(BlockHeaderV2) + blkV2->_size))
		{
			return false;
		}

		//将文件头后面的数据进行解压
		buffer = WTSCmpHelper::uncompress_data(content.data() + BLOCK_HEADERV2_SIZE, blkV2->_size);
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


/**
 * @brief 将时间字符串转换为整数时间
 * @param strTime 时间字符串，格式如"10:30:25"
 * @param bKeepSec 是否保留秒数，默认为false
 * @return uint32_t 转换后的整数时间，如果bKeepSec为false则返回小时分钟（HHMM），否则返回小时分钟秒（HHMMSS）
 * 
 * @details 该函数将包含冒号的时间字符串转换为整数格式的时间。
 * 首先移除字符串中的冒号，然后将其转换为整数。
 * 如果bKeepSec为false且结果大于10000（即包含秒），则会将结果除以100以移除秒数。
 */
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

	uint32_t ret = strtoul(str.c_str(), NULL, 10);
	if (ret > 10000 && !bKeepSec)
		ret /= 100;

	return ret;
}

/**
 * @brief 将日期字符串转换为整数日期
 * @param strDate 日期字符串，支持格式如"2020/03/30"或"2020-03-30"
 * @return uint32_t 转换后的整数日期，格式为YYYYMMDD
 * 
 * @details 该函数将日期字符串转换为整数格式的日期。
 * 首先根据分隔符（"/"或"-"）将字符串分割为年、月、日部分，
 * 然后将其组合成YYYYMMDD格式的整数。如果月或日为单位数，会自动添加前导零。
 */
uint32_t strToDate(const char* strDate)
{
	StringVector ay = StrUtil::split(strDate, "/");
	if (ay.size() == 1)
		ay = StrUtil::split(strDate, "-");
	std::stringstream ss;
	if (ay.size() > 1)
	{
		auto pos = ay[2].find(" ");
		if (pos != std::string::npos)
			ay[2] = ay[2].substr(0, pos);
		ss << ay[0] << (ay[1].size() == 1 ? "0" : "") << ay[1] << (ay[2].size() == 1 ? "0" : "") << ay[2];
	}
	else
		ss << ay[0];

	return strtoul(ss.str().c_str(), NULL, 10);
}

/**
 * @brief 将二进制K线数据导出为CSV格式
 * @param binFolder 二进制数据文件夹路径
 * @param csvFolder CSV输出文件夹路径
 * @param strFilter 合约过滤器，支持通配符，默认为空（处理所有合约）
 * @param cbLogger 日志回调函数，默认为NULL
 * 
 * @details 该函数遍历指定目录中的.dsb文件，将其中的K线数据转换为CSV格式并存储到目标文件夹。
 * 对于每个文件，首先检查是否是K线数据，然后读取并处理数据块，最后将数据写入CSV文件。
 * 可以通过strFilter参数指定要处理的合约。
 */
void dump_bars(WtString binFolder, WtString csvFolder, WtString strFilter /* = "" */, FuncLogCallback cbLogger /* = NULL */)
{
	std::string srcFolder = StrUtil::standardisePath(binFolder);
	if (!BoostFile::exists(srcFolder.c_str()))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("目录%s不存在", binFolder).c_str());
		return;
	}

	if (!BoostFile::exists(csvFolder))
		BoostFile::create_directories(csvFolder);

	boost::filesystem::path myPath(srcFolder);
	boost::filesystem::directory_iterator endIter;
	for (boost::filesystem::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		if (boost::filesystem::is_directory(iter->path()))
			continue;

		if (iter->path().extension() != ".dsb")
			continue;

		const std::string& path = iter->path().string();

		std::string fileCode = iter->path().stem().string();

		if (cbLogger)
			cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

		std::string buffer;
		BoostFile::read_file_contents(path.c_str(), buffer);
		if (buffer.size() < sizeof(HisKlineBlock))
		{
			if (cbLogger)
				cbLogger(StrUtil::printf("文件%s头部校验失败", binFolder).c_str());
			continue;
		}

		BlockHeader* bHeader = (BlockHeader*)buffer.data();

		if(bHeader->_type < BT_HIS_Minute1 || bHeader->_type > BT_HIS_Day)
		{
			if (cbLogger)
				cbLogger(StrUtil::printf("文件%s不是K线数据，跳过转换", binFolder).c_str());
			continue;
		}

		bool isDay = (bHeader->_type == BT_HIS_Day);

		proc_block_data(buffer, true, false);		

		auto kcnt = buffer.size() / sizeof(WTSBarStruct);
		if (kcnt <= 0)
			continue;

		std::string filename = StrUtil::standardisePath(csvFolder);
		filename += fileCode;
		filename += ".csv";

		if (cbLogger)
			cbLogger(StrUtil::printf("正在写入%s...", filename.c_str()).c_str());

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

		BoostFile::write_file_contents(filename.c_str(), ss.str().c_str(), (uint32_t)ss.str().size());

		if (cbLogger)
			cbLogger(StrUtil::printf("%s写入完成,共%u条bar", filename.c_str(), kcnt).c_str());
	}

	if (cbLogger)
		cbLogger(StrUtil::printf("目录%s全部导出完成...", binFolder).c_str());
}

/**
 * @brief 将二进制Tick数据导出为CSV格式
 * @param binFolder 二进制数据文件夹路径
 * @param csvFolder CSV输出文件夹路径
 * @param strFilter 合约过滤器，支持通配符，默认为空（处理所有合约）
 * @param cbLogger 日志回调函数，默认为NULL
 * 
 * @details 该函数遍历指定目录中的.dsb文件，将其中的Tick数据转换为CSV格式并存储到目标文件夹。
 * 对于每个文件，首先检查是否是Tick数据，然后读取并处理数据块，最后将数据写入CSV文件。
 * 可以通过strFilter参数指定要处理的合约。
 */
void dump_ticks(WtString binFolder, WtString csvFolder, WtString strFilter /* = "" */, FuncLogCallback cbLogger /* = NULL */)
{
	std::string srcFolder = StrUtil::standardisePath(binFolder);
	if (!BoostFile::exists(srcFolder.c_str()))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("目录%s不存在", binFolder).c_str());
		return;
	}

	if (!BoostFile::exists(csvFolder))
		BoostFile::create_directories(csvFolder);

	boost::filesystem::path myPath(srcFolder);
	boost::filesystem::directory_iterator endIter;
	for (boost::filesystem::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		if (boost::filesystem::is_directory(iter->path()))
			continue;

		if (iter->path().extension() != ".dsb")
			continue;

		const std::string& path = iter->path().string();

		std::string fileCode = iter->path().stem().string();

		if (cbLogger)
			cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

		std::string buffer;
		BoostFile::read_file_contents(path.c_str(), buffer);
		if (buffer.size() < sizeof(HisTickBlock))
		{
			if (cbLogger)
				cbLogger(StrUtil::printf("文件%s头部校验失败", binFolder).c_str());
			continue;
		}

		proc_block_data(buffer, false, false);

		auto tcnt = buffer.size() / sizeof(WTSTickStruct);
		if (tcnt <= 0)
			continue;

		std::string filename = StrUtil::standardisePath(csvFolder);
		filename += fileCode;
		filename += ".csv";

		if (cbLogger)
			cbLogger(StrUtil::printf("正在写入%s...", filename.c_str()).c_str());

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

		BoostFile::write_file_contents(filename.c_str(), ss.str().c_str(), (uint32_t)ss.str().size());

		if (cbLogger)
			cbLogger(StrUtil::printf("%s写入完成,共%u条tick数据", filename.c_str(), tcnt).c_str());
	}

	if (cbLogger)
		cbLogger(StrUtil::printf("目录%s全部导出完成...", binFolder).c_str());
}

/**
 * @brief 将CSV格式的K线数据转换为二进制格式
 * @param csvFolder CSV数据文件夹路径
 * @param binFolder 二进制输出文件夹路径
 * @param period K线周期，如"m1"/"d1"
 * @param cbLogger 日志回调函数，默认为NULL
 * 
 * @details 该函数遍历指定目录中的CSV文件，将其中的K线数据转换为二进制格式并存储到目标文件夹。
 * 对于每个文件，首先读取CSV数据，然后将其转换为K线结构数组，最后压缩并写入二进制文件。
 * 输出文件的类型由period参数决定，支持分钟线、日线等不同周期的K线数据。
 */
void trans_csv_bars(WtString csvFolder, WtString binFolder, WtString period, FuncLogCallback cbLogger /* = NULL */)
{
	if (!BoostFile::exists(csvFolder))
		return;

	if (!BoostFile::exists(binFolder))
		BoostFile::create_directories(binFolder);

	WTSKlinePeriod kp = KP_DAY;
	if (wt_stricmp(period, "m1") == 0)
		kp = KP_Minute1;
	else if (wt_stricmp(period, "m5") == 0)
		kp = KP_Minute5;
	else
		kp = KP_DAY;

	boost::filesystem::path myPath(csvFolder);
	boost::filesystem::directory_iterator endIter;
	for (boost::filesystem::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		if (boost::filesystem::is_directory(iter->path()))
			continue;

		if (iter->path().extension() != ".csv")
			continue;

		const std::string& path = iter->path().string();

		if(cbLogger)
			cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

		CsvReader reader(",");
		if(!reader.load_from_file(path.c_str()))
		{
			if (cbLogger)
				cbLogger(StrUtil::printf("读取数据文件%s失败...", path.c_str()).c_str());
			continue;
		}

		std::vector<WTSBarStruct> bars;

		while(reader.next_row())
		{
			//逐行读取
			WTSBarStruct bs;
			bs.date = strToDate(reader.get_string("date"));
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
					cbLogger(StrUtil::printf("已读取数据%u条", bars.size()).c_str());
			}
		}
		if (cbLogger)
			cbLogger(StrUtil::printf("数据文件%s全部读取完成,共%u条", path.c_str(), bars.size()).c_str());

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

		BoostFile bf;
		if (bf.create_new_file(filename.c_str()))
		{
			bf.write_file(&kBlock, sizeof(HisKlineBlockV2));
		}
		bf.write_file(cmprsData);
		bf.close_file();
		if (cbLogger)
			cbLogger(StrUtil::printf("数据已转储至%s", filename.c_str()).c_str());
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

/**
 * @brief 读取二进制Tick数据文件（.dsb格式）
 * @param tickFile Tick数据文件路径
 * @param cb Tick数据获取回调函数
 * @param cbCnt 数据计数回调函数
 * @param cbLogger 日志回调函数，默认为NULL
 * @return WtUInt32 读取到的数据数量
 * 
 * @details 该函数从指定的.dsb格式文件中读取Tick数据，并通过回调函数返回给调用者。
 * 首先读取文件内容，然后处理数据块（包括解压缩和版本转换），
 * 最后通过cbCnt回调函数通知总数量，并通过cb回调函数返回数据。
 */
WtUInt32 read_dsb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger /* = NULL */)
{
	std::string path = tickFile;

	if (cbLogger)
		cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

	std::string content;
	BoostFile::read_file_contents(path.c_str(), content);
	if (content.size() < sizeof(HisTickBlock))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("文件%s头部校验失败", tickFile).c_str());
		return 0;
	}

	proc_block_data(content, false, false);

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSTickStruct);

	cbCnt(tcnt);
	cb((WTSTickStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(StrUtil::printf("%s读取完成,共%u条tick数据", tickFile, tcnt).c_str());

	return (WtUInt32)tcnt;
}

/**
 * @brief 读取二进制委托明细数据文件（.dsb格式）
 * @param dataFile 委托明细数据文件路径
 * @param cb 委托明细数据获取回调函数
 * @param cbCnt 数据计数回调函数
 * @param cbLogger 日志回调函数，默认为NULL
 * @return WtUInt32 读取到的数据数量
 * 
 * @details 该函数从指定的.dsb格式文件中读取委托明细数据，并通过回调函数返回给调用者。
 * 首先读取文件内容，然后处理数据块（包括解压缩和版本转换），
 * 最后通过cbCnt回调函数通知总数量，并通过cb回调函数返回数据。
 */
WtUInt32 read_dsb_order_details(WtString dataFile, FuncGetOrdDtlCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger/* = NULL*/)
{
	std::string path = dataFile;

	if (cbLogger)
		cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

	std::string content;
	BoostFile::read_file_contents(path.c_str(), content);
	if (content.size() < sizeof(HisOrdDtlBlock))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("文件%s头部校验失败", dataFile).c_str());
		return 0;
	}

	proc_block_data(content, false, false);

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSOrdDtlStruct);

	cbCnt(tcnt);
	cb((WTSOrdDtlStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(StrUtil::printf("%s读取完成,共%u条order detail数据", dataFile, tcnt).c_str());

	return (WtUInt32)tcnt;
}

/**
 * @brief 读取二进制委托队列数据文件（.dsb格式）
 * @param dataFile 委托队列数据文件路径
 * @param cb 委托队列数据获取回调函数
 * @param cbCnt 数据计数回调函数
 * @param cbLogger 日志回调函数，默认为NULL
 * @return WtUInt32 读取到的数据数量
 * 
 * @details 该函数从指定的.dsb格式文件中读取委托队列数据，并通过回调函数返回给调用者。
 * 首先读取文件内容，然后处理数据块（包括解压缩和版本转换），
 * 最后通过cbCnt回调函数通知总数量，并通过cb回调函数返回数据。
 */
WtUInt32 read_dsb_order_queues(WtString dataFile, FuncGetOrdQueCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger/* = NULL*/)
{
	std::string path = dataFile;

	if (cbLogger)
		cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

	std::string content;
	BoostFile::read_file_contents(path.c_str(), content);
	if (content.size() < sizeof(HisOrdQueBlock))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("文件%s头部校验失败", dataFile).c_str());
		return 0;
	}

	proc_block_data(content, false, false);

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSOrdQueStruct);

	cbCnt(tcnt);
	cb((WTSOrdQueStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(StrUtil::printf("%s读取完成,共%u条order queue数据", dataFile, tcnt).c_str());

	return (WtUInt32)tcnt;
}

/**
 * @brief 读取二进制成交数据文件（.dsb格式）
 * @param dataFile 成交数据文件路径
 * @param cb 成交数据获取回调函数
 * @param cbCnt 数据计数回调函数
 * @param cbLogger 日志回调函数，默认为NULL
 * @return WtUInt32 读取到的数据数量
 * 
 * @details 该函数从指定的.dsb格式文件中读取成交数据，并通过回调函数返回给调用者。
 * 首先读取文件内容，然后处理数据块（包括解压缩和版本转换），
 * 最后通过cbCnt回调函数通知总数量，并通过cb回调函数返回数据。
 */
WtUInt32 read_dsb_transactions(WtString dataFile, FuncGetTransCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger/* = NULL*/)
{
	std::string path = dataFile;

	if (cbLogger)
		cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

	std::string content;
	BoostFile::read_file_contents(path.c_str(), content);
	if (content.size() < sizeof(HisTransBlock))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("文件%s头部校验失败", dataFile).c_str());
		return 0;
	}

	proc_block_data(content, false, false);

	if (content.empty())
	{
		cbCnt(0);
		return 0;
	}

	auto tcnt = content.size() / sizeof(WTSTransStruct);

	cbCnt(tcnt);
	cb((WTSTransStruct*)content.data(), tcnt, true);

	if (cbLogger)
		cbLogger(StrUtil::printf("%s读取完成,共%u条transaction数据", dataFile, tcnt).c_str());

	return (WtUInt32)tcnt;
}

/**
 * @brief 读取二进制K线数据文件（.dsb格式）
 * @param barFile K线数据文件路径
 * @param cb K线数据获取回调函数
 * @param cbCnt 数据计数回调函数
 * @param cbLogger 日志回调函数
 * @return WtUInt32 读取到的数据数量
 * 
 * @details 该函数从指定的.dsb格式文件中读取K线数据，并通过回调函数返回给调用者。
 * 首先读取文件内容，然后处理数据块（包括解压缩和版本转换），
 * 最后通过cbCnt回调函数通知总数量，并通过cb回调函数返回数据。
 */
WtUInt32 read_dsb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger )
{
	std::string path = barFile;
	if (cbLogger)
		cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

	std::string content;
	BoostFile::read_file_contents(path.c_str(), content);
	if (content.size() < sizeof(HisKlineBlock))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("文件%s头部校验失败", barFile).c_str());
		return 0;
	}

	proc_block_data(content, true, false);

	if(content.empty())
	{
		cbCnt(0);
		return 0;
	}


	auto kcnt = content.size() / sizeof(WTSBarStruct);
	cbCnt(kcnt);
	cb((WTSBarStruct*)content.data(), kcnt, true);

	if (cbLogger)
		cbLogger(StrUtil::printf("%s读取完成,共%u条bar", barFile, kcnt).c_str());

	return (WtUInt32)kcnt;
}

/**
 * @brief 读取内存映射二进制K线数据文件（.dmb格式）
 * @param barFile K线数据文件路径
 * @param cb K线数据获取回调函数
 * @param cbCnt 数据计数回调函数
 * @param cbLogger 日志回调函数
 * @return WtUInt32 读取到的数据数量
 * 
 * @details 该函数从指定的.dmb格式文件中读取K线数据，并通过回调函数返回给调用者。
 * .dmb格式是内存映射二进制格式，读取效率更高。
 * 首先读取文件内容，然后处理数据块，
 * 最后通过cbCnt回调函数通知总数量，并通过cb回调函数返回数据。
 */
WtUInt32 read_dmb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger)
{
	std::string path = barFile;

	std::string buffer;
	BoostFile::read_file_contents(path.c_str(), buffer);
	if (buffer.size() < sizeof(RTKlineBlock))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("文件%s头部校验失败", barFile).c_str());
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
		cbLogger(StrUtil::printf("%s读取完成,共%u条bar", barFile, kcnt).c_str());

	return (WtUInt32)kcnt;
}

/**
 * @brief 读取内存映射二进制Tick数据文件（.dmb格式）
 * @param tickFile Tick数据文件路径
 * @param cb Tick数据获取回调函数
 * @param cbCnt 数据计数回调函数
 * @param cbLogger 日志回调函数，默认为NULL
 * @return WtUInt32 读取到的数据数量
 * 
 * @details 该函数从指定的.dmb格式文件中读取Tick数据，并通过回调函数返回给调用者。
 * .dmb格式是内存映射二进制格式，读取效率更高。
 * 首先读取文件内容，然后处理数据块，
 * 最后通过cbCnt回调函数通知总数量，并通过cb回调函数返回数据。
 */
WtUInt32 read_dmb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger /* = NULL */)
{
	std::string path = tickFile;

	if (cbLogger)
		cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

	std::string buffer;
	BoostFile::read_file_contents(path.c_str(), buffer);
	if (buffer.size() < sizeof(RTTickBlock))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("文件%s头部校验失败", tickFile).c_str());
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
		cbLogger(StrUtil::printf("%s读取完成,共%u条tick数据", tickFile, tcnt).c_str());

	return (WtUInt32)tcnt;
}


/**
 * @brief 对K线数据进行重采样
 * @param barFile 输入K线数据文件路径
 * @param cb K线数据获取回调函数，用于返回重采样后的数据
 * @param cbCnt 数据计数回调函数
 * @param fromTime 开始时间
 * @param endTime 结束时间
 * @param period 目标K线周期，如"m5"/"d1"
 * @param times 周期倍数，如将分钟线采样为5分钟线，则为5
 * @param sessInfo 交易时段信息
 * @param cbLogger 日志回调函数，默认为NULL
 * @param bAlignSec 是否按秒对齐，默认为false
 * @return WtUInt32 重采样后的数据数量
 * 
 * @details 该函数将指定文件中的K线数据进行重采样，生成新的周期的K线数据。
 * 首先读取原始数据，然后根据指定的周期和倍数进行采样，
 * 最后通过cb回调函数返回采样后的数据。
 * 该函数支持将分钟线采样为更高周期的K线，如将分钟线采样为5分钟线、日线等。
 */
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
	else
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("周期%s不是基础周期...", period).c_str());
		return 0;
	}

	bool isDay = (kp == KP_DAY);

	if(isDay)
	{
		if(fromTime >= 100000000 || endTime > 100000000)
		{
			if (cbLogger)
				cbLogger("日线基础数据的开始时间结束时间应为日期，格式如yyyymmdd");
			return 0;
		}
	}
	else
	{
		if (fromTime < 100000000 || endTime < 100000000)
		{
			if (cbLogger)
				cbLogger("分钟线基础数据的开始时间结束时间应为时间，格式如yyyymmddHHMM");
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
				cbLogger("交易时间模板解析失败");
			return 0;
		}

		int32_t offset = root["offset"].GetInt();

		sInfo = WTSSessionInfo::create("tmp", "tmp", offset);

		if (!root["auction"].IsNull())
		{
			const rj::Value& jAuc = root["auction"];
			sInfo->setAuctionTime(jAuc["from"].GetUint(), jAuc["to"].GetUint());
		}

		const rj::Value& jSecs = root["sections"];
		if (jSecs.IsNull() || !jSecs.IsArray())
		{
			if (cbLogger)
				cbLogger("交易时间模板格式错误");
			return 0;
		}

		for (const rj::Value& jSec : jSecs.GetArray())
		{
			sInfo->addTradingSection(jSec["from"].GetUint(), jSec["to"].GetUint());
		}
	}

	std::string path = barFile;
	if (cbLogger)
		cbLogger(StrUtil::printf("正在读取数据文件%s...", path.c_str()).c_str());

	std::string buffer;
	BoostFile::read_file_contents(path.c_str(), buffer);
	if (buffer.size() < sizeof(HisKlineBlock))
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("文件%s头部校验失败", barFile).c_str());
		return 0;
	}

	proc_block_data(buffer, true, false);

	auto kcnt = buffer.size() / sizeof(WTSBarStruct);
	if (kcnt <= 0)
	{
		if (cbLogger)
			cbLogger(StrUtil::printf("%s数据为空", barFile).c_str());
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
			cbLogger("没有找到指定时间范围的K线");
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
			cbLogger("K线重采样失败");
		return 0;
	}

	uint32_t newCnt = kline->size();
	cbCnt(newCnt);
	cb(&kline->getDataRef().at(0),newCnt, true);

	if (cbLogger)
		cbLogger(StrUtil::printf("%s重采样完成,共将%u条bar重采样为%u条新bar", barFile, hitCnt, newCnt).c_str());

	
	kline->release();
	sInfo->release();
	slice->release();

	return (WtUInt32)newCnt;
}

/**
 * @brief 将K线数据存储为二进制文件
 * @param barFile 输出文件路径
 * @param firstBar 第一个K线数据结构指针，数据数组的起始地址
 * @param count 数据数量
 * @param period K线周期，如"m1"/"d1"
 * @param cbLogger 日志回调函数，默认为NULL
 * @return bool 存储是否成功
 * 
 * @details 该函数将内存中的K线数据数组存储为二进制文件（.dsb格式）。
 * 首先检查数据有效性，然后将数据复制到缓冲区，
 * 根据周期类型设置数据块类型，将数据压缩后写入文件。
 * 该函数支持存储分钟线、日线等不同周期的K线数据。
 */
bool store_bars(WtString barFile, WTSBarStruct* firstBar, int count, WtString period, FuncLogCallback cbLogger /* = NULL */)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger("K线数据条数为0");
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
			cbLogger("周期只能为m1、m5或d");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSBarStruct)*count);
	WTSBarStruct* bars = (WTSBarStruct*)buffer.c_str();
	memcpy(bars, firstBar, sizeof(WTSBarStruct)*count);

	if (cbLogger)
		cbLogger("K线数据已经读取完成，准备写入文件");

	std::string content;
	content.resize(sizeof(HisKlineBlockV2));
	HisKlineBlockV2* block = (HisKlineBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = bType;
	std::string cmp_data = WTSCmpHelper::compress_data(bars, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	BoostFile bf;
	if (bf.create_new_file(barFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger("K线数据写入文件成功");
	return true;
}

/**
 * @brief 将Tick数据存储为二进制文件
 * @param tickFile 输出文件路径
 * @param firstTick 第一个Tick数据结构指针，数据数组的起始地址
 * @param count 数据数量
 * @param cbLogger 日志回调函数，默认为NULL
 * @return bool 存储是否成功
 * 
 * @details 该函数将内存中的Tick数据数组存储为二进制文件（.dsb格式）。
 * 首先检查数据有效性，然后将数据复制到缓冲区，
 * 设置数据块类型为BT_HIS_Ticks，将数据压缩后写入文件。
 * 该函数用于存储各种金融市场的Tick数据，包括股票、期货等。
 */
bool store_ticks(WtString tickFile, WTSTickStruct* firstTick, int count, FuncLogCallback cbLogger/* = NULL*/)
{
	if (count == 0)
	{
		if (cbLogger)
			cbLogger("Tick数据条数为0");
		return false;
	}

	std::string buffer;
	buffer.resize(sizeof(WTSTickStruct)*count);
	WTSTickStruct* ticks = (WTSTickStruct*)buffer.c_str();
	memcpy(ticks, firstTick, sizeof(WTSTickStruct)*count);

	if (cbLogger)
		cbLogger("Tick数据已经读取完成，准备写入文件");

	std::string content;
	content.resize(sizeof(HisTickBlockV2));
	HisTickBlockV2* block = (HisTickBlockV2*)content.data();
	strcpy(block->_blk_flag, BLK_FLAG);
	block->_version = BLOCK_VERSION_CMP_V2;
	block->_type = BT_HIS_Ticks;
	std::string cmp_data = WTSCmpHelper::compress_data(ticks, buffer.size());
	block->_size = cmp_data.size();
	content.append(cmp_data);

	BoostFile bf;
	if (bf.create_new_file(tickFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger("Tick数据写入文件成功");

	return true;
}

/**
 * @brief 将委托明细数据存储为二进制文件（股票Level2数据）
 * @param tickFile 输出文件路径
 * @param firstItem 第一个委托明细数据结构指针，数据数组的起始地址
 * @param count 数据数量
 * @param cbLogger 日志回调函数，默认为NULL
 * @return bool 存储是否成功
 * 
 * @details 该函数将内存中的委托明细数据数组存储为二进制文件（.dsb格式）。
 * 首先检查数据有效性，然后将数据复制到缓冲区，
 * 设置数据块类型为BT_HIS_OrdDetail，将数据压缩后写入文件。
 * 该函数主要用于存储股票Level2数据中的委托明细信息。
 */
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

	BoostFile bf;
	if (bf.create_new_file(tickFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger("Writing order details succeed");

	return true;
}

/**
 * @brief 将委托队列数据存储为二进制文件（股票Level2数据）
 * @param tickFile 输出文件路径
 * @param firstItem 第一个委托队列数据结构指针，数据数组的起始地址
 * @param count 数据数量
 * @param cbLogger 日志回调函数，默认为NULL
 * @return bool 存储是否成功
 * 
 * @details 该函数将内存中的委托队列数据数组存储为二进制文件（.dsb格式）。
 * 首先检查数据有效性，然后将数据复制到缓冲区，
 * 设置数据块类型为BT_HIS_OrdQueue，将数据压缩后写入文件。
 * 该函数主要用于存储股票Level2数据中的委托队列信息。
 */
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

	BoostFile bf;
	if (bf.create_new_file(tickFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger("Writing order queues to file succeedd");

	return true;
}

/**
 * @brief 将成交数据存储为二进制文件（股票Level2数据）
 * @param tickFile 输出文件路径
 * @param firstItem 第一个成交数据结构指针，数据数组的起始地址
 * @param count 数据数量
 * @param cbLogger 日志回调函数，默认为NULL
 * @return bool 存储是否成功
 * 
 * @details 该函数将内存中的成交数据数组存储为二进制文件（.dsb格式）。
 * 首先检查数据有效性，然后将数据复制到缓冲区，
 * 设置数据块类型为BT_HIS_Trnsctn，将数据压缩后写入文件。
 * 该函数主要用于存储股票Level2数据中的成交信息。
 */
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

	BoostFile bf;
	if (bf.create_new_file(tickFile))
	{
		bf.write_file(content);
	}
	bf.close_file();

	if (cbLogger)
		cbLogger("Write transactions to file succeedd");

	return true;
}