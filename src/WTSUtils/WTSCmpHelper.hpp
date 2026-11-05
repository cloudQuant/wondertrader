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
#include <atomic>
#include <stdexcept>
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
	/**
	 * @brief 解压资源预算（决策 K37=B，2026-11-04）
	 *
	 * 默认绝对上限 1 GiB、压缩比上限 1000:1；任一值为 0 表示不启用该项。预算按进程（模块）生效，
	 * 解压前依据帧头声明的内容大小检查，超限或大小未知/非法时抛出 std::runtime_error，不分配缓冲。
	 */
	static void set_uncompress_budget(uint64_t maxBytes, uint64_t maxRatio)
	{
		budget_max_bytes().store(maxBytes, std::memory_order_relaxed);
		budget_max_ratio().store(maxRatio, std::memory_order_relaxed);
		budget_refuse_all().store(false, std::memory_order_relaxed);
	}

	/**
	 * @brief 解压预算配置非法时的 fail-closed 状态（BUG-R132-001 / K37）
	 *
	 * 宿主给出的预算配置不能解析时，本模块不得回退到默认或无限预算：置位后任何解压都抛出
	 * std::runtime_error（调用方按既有错误路径记 ERROR、不缓存）。之后成功应用一份合法预算
	 * （set_uncompress_budget）即解除。
	 */
	static void refuse_uncompress(bool refuse)
	{
		budget_refuse_all().store(refuse, std::memory_order_relaxed);
	}

	static bool uncompress_refused() { return budget_refuse_all().load(std::memory_order_relaxed); }

	static uint64_t uncompress_max_bytes() { return budget_max_bytes().load(std::memory_order_relaxed); }
	static uint64_t uncompress_max_ratio() { return budget_max_ratio().load(std::memory_order_relaxed); }

	static std::string uncompress_data(const void* data, size_t dataLen)
	{
		std::string desBuf;
		if (data == NULL || dataLen == 0)
			throw std::runtime_error("compressed data is empty");
		if (uncompress_refused())
			throw std::runtime_error("decompression is refused: the configured uncompress budget is invalid");
		unsigned long long const desLen = ZSTD_getFrameContentSize(data, dataLen);
		if (desLen == ZSTD_CONTENTSIZE_ERROR || desLen == ZSTD_CONTENTSIZE_UNKNOWN)
			throw std::runtime_error("compressed frame does not declare a valid content size");
		const uint64_t maxBytes = uncompress_max_bytes();
		if (maxBytes != 0 && desLen > maxBytes)
			throw std::runtime_error("declared decompressed size exceeds the absolute budget");
		const uint64_t maxRatio = uncompress_max_ratio();
		// desLen / dataLen > maxRatio, evaluated without multiplication overflow.
		if (maxRatio != 0 && (desLen / maxRatio > dataLen || (desLen / maxRatio == dataLen && desLen % maxRatio != 0)))
			throw std::runtime_error("declared decompressed size exceeds the compression ratio budget");
		if (desLen > static_cast<unsigned long long>(desBuf.max_size()))
			throw std::runtime_error("declared decompressed size is not addressable");
		desBuf.resize((std::size_t)desLen, 0);
		size_t const dSize = ZSTD_decompress((void*)desBuf.data(), (size_t)desLen, data, dataLen);
		if (dSize != desLen)
			throw std::runtime_error("uncompressed data size does not match calculated data size");
		return desBuf;
	}

private:
	static std::atomic<uint64_t>& budget_max_bytes()
	{
		static std::atomic<uint64_t> value{ 1ull << 30 };
		return value;
	}

	static std::atomic<uint64_t>& budget_max_ratio()
	{
		static std::atomic<uint64_t> value{ 1000 };
		return value;
	}

	static std::atomic<bool>& budget_refuse_all()
	{
		static std::atomic<bool> value{ false };
		return value;
	}
};

