/*!
 * \file WTSCmpHelper.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据压缩辅助类,利用zstdlib压缩
 *
 * 本文件定义了WTSCmpHelper类，内部封装了基于zstd库的高效压缩和解压缩功能。
 * zstd库是一个速度快、压缩比高的开源压缩库，在数据存储和传输中有广泛应用。
 * 在WonderTrader中，该类用于压缩和解压缩各种类型的数据，供其他模块调用。
 */
#pragma once
#include <string>
#include <stdint.h>

#include "../WTSUtils/zstdlib/zstd.h"

/**
 * @brief 数据压缩辅助类
 * 
 * 提供基于zstd库的数据压缩和解压缩功能的静态工具类
 * 包含了同名的压缩和解压缩方法，为WonderTrader系统提供高效的数据压缩能力
 */
class WTSCmpHelper
{
public:
	/**
	 * @brief 对数据进行压缩
	 * 
	 * 使用zstd库将指定数据压缩并返回压缩后的字符串
	 * 
	 * @param data 要压缩的数据指针
	 * @param dataLen 数据长度（字节数）
	 * @param uLevel 压缩级别，范围为1-22，值越大压缩比越高但速度越慢，默认为1
	 * @return std::string 返回压缩后的数据字符串
	 */
	static std::string compress_data(const void* data, size_t dataLen, uint32_t uLevel = 1)
	{
		std::string desBuf;
		std::size_t const desLen = ZSTD_compressBound(dataLen);
		desBuf.resize(desLen, 0);
		size_t const cSize = ZSTD_compress((void*)desBuf.data(), desLen, data, dataLen, uLevel);
		desBuf.resize(cSize);
		return desBuf;
	}

	/**
	 * @brief 对压缩数据进行解压缩
	 * 
	 * 使用zstd库解压缩指定的压缩数据，并返回解压缩后的字符串
	 * 如果解压缩后的数据大小与计算的大小不匹配，将抛出异常
	 * 
	 * @param data 要解压缩的压缩数据指针
	 * @param dataLen 压缩数据长度（字节数）
	 * @return std::string 返回解压缩后的数据字符串
	 * @throw std::runtime_error 当解压缩后的数据大小与预期不符时抛出异常
	 */
	static std::string uncompress_data(const void* data, size_t dataLen)
	{
		std::string desBuf;
		unsigned long long const desLen = ZSTD_getFrameContentSize(data, dataLen);
		desBuf.resize((std::size_t)desLen, 0);
		size_t const dSize = ZSTD_decompress((void*)desBuf.data(), (size_t)desLen, data, dataLen);
		if (dSize != desLen)
			throw std::runtime_error("uncompressed data size does not match calculated data size");
		return desBuf;
	}
};

