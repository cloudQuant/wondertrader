/*!
 * \file WtHelper.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader辅助工具类头文件
 * \details 定义了WtHelper类，提供了一系列辅助工具函数，包括路径管理、时间管理等功能
 */
#pragma once
#include <string>
#include <stdint.h>

/**
 * @brief WonderTrader辅助工具类
 * @details 提供了一系列静态辅助函数，用于路径管理、时间处理等基础功能
 */
class WtHelper
{
public:
	/**
	 * @brief 获取当前工作目录
	 * @return 当前工作目录的字符串表示
	 */
	static std::string getCWD();

	/**
	 * @brief 获取模块路径
	 * @param moduleName 模块名称
	 * @param subDir 子目录
	 * @param isCWD 是否基于当前工作目录，默认为true
	 * @return 模块路径的字符串表示
	 */
	static std::string getModulePath(const char* moduleName, const char* subDir, bool isCWD = true);

	/**
	 * @brief 获取基础目录
	 * @return 基础目录的字符串表示
	 */
	static const char* getBaseDir();
	/**
	 * @brief 获取输出目录
	 * @return 输出目录的字符串表示
	 */
	static const char* getOutputDir();
	/**
	 * @brief 获取策略数据目录
	 * @return 策略数据目录的字符串表示
	 */
	static const char* getStraDataDir();
	/**
	 * @brief 获取策略用户数据目录
	 * @return 策略用户数据目录的字符串表示
	 */
	static const char* getStraUsrDatDir();

	/**
	 * @brief 获取组合策略目录
	 * @return 组合策略目录的字符串表示
	 */
	static const char* getPortifolioDir();

	/**
	 * @brief 设置当前时间
	 * @param date 日期，格式为YYYYMMDD
	 * @param time 时间，格式为HHMM或HHMMSS
	 * @param secs 秒数，包含毫秒，默认为0
	 */
	static inline void setTime(uint32_t date, uint32_t time, uint32_t secs = 0)
	{
		_cur_date = date;
		_cur_time = time;
		_cur_secs = secs;
	}

	/**
	 * @brief 设置当前交易日
	 * @param tDate 交易日，格式为YYYYMMDD
	 */
	static inline void setTDate(uint32_t tDate){ _cur_tdate = tDate; }

	/**
	 * @brief 获取当前日期
	 * @return 当前日期，格式为YYYYMMDD
	 */
	static inline uint32_t getDate(){ return _cur_date; }
	
	/**
	 * @brief 获取当前时间
	 * @return 当前时间，格式为HHMMSS
	 */
	static inline uint32_t getTime(){ return _cur_time; }
	
	/**
	 * @brief 获取当前秒数
	 * @return 当前秒数，包含毫秒
	 */
	static inline uint32_t getSecs(){ return _cur_secs; }
	
	/**
	 * @brief 获取当前交易日
	 * @return 当前交易日，格式为YYYYMMDD
	 */
	static inline uint32_t getTradingDate(){ return _cur_tdate; }

	/**
	 * @brief 获取实例目录
	 * @return 实例目录的字符串引用
	 */
	static const std::string& getInstDir() { return _inst_dir; }
	
	/**
	 * @brief 设置实例目录
	 * @param inst_dir 实例目录路径
	 */
	static void setInstDir(const char* inst_dir){ _inst_dir = inst_dir; }

	/**
	 * @brief 设置生成文件输出目录
	 * @param gen_dir 生成文件输出目录路径
	 */
	static void setGenerateDir(const char* gen_dir) { _gen_dir = gen_dir; }

private:
	static uint32_t		_cur_date;	///< 当前日期，格式为YYYYMMDD
	static uint32_t		_cur_time;	///< 当前时间，格式为HHMMSS
	static uint32_t		_cur_secs;	///< 当前秒数，包含毫秒
	static uint32_t		_cur_tdate;	///< 当前交易日，格式为YYYYMMDD
	static std::string	_inst_dir;	///< 实例所在目录
	static std::string	_gen_dir;	///< 生成文件输出目录
};

