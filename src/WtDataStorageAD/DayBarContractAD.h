/*!
 * \file DayBarContractAD.h
 * \project	WonderTrader
 *
 * \brief AD 存储日线 time 字段对外合同（迭代03 卡3 3c=C，2026-11-04 用户裁定；R4-BUG-13）
 *
 * - 对外合同：日线 WTSBarStruct::time == 0，date 为交易日（与通用 WtDataWriter 一致）。
 * - 新数据：WtDataWriterAD 新建日线 time 写 0；新建 cache_d1.dmb 的 RT 头 _version = 2。
 * - 版本标记：cache_d1.dmb 头 _version
 *     1 = 旧格式，日线 time 可能等于交易日（旧 AD writer 写法）；
 *     2 = 全部条目 time = 0。
 *   writer 打开已有版本 1 的日线缓存时，在该文件的跨进程发布锁与结构性日志内把全部条目 time
 *   原地改写为 0，最后把头版本改为 2（卡13 多 writer 协议不变）。tick/m1/m5 缓存版本仍为 1。
 * - 兼容读取：reader 接受日线缓存版本 1 与 2；从日线缓存或 LMDB day 库读出的日线一律把 time
 *   归一为 0。LMDB 值没有逐条版本，旧 writer 写入的 day 记录（time=交易日）只能在读端归一。
 * - 未知版本（非 1/2）仍按原规则拒绝（fail-closed）。旧二进制读者/写者遇到版本 2 的日线缓存会
 *   以“unsupported version”拒绝，须先升级再混跑。
 */
#pragma once
#include <stdint.h>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include "../Includes/WTSStruct.h"

#define BLOCK_VERSION_DAY_T0	2	//日线 RT 缓存：time=0 合同

namespace wt_ad_daybar
{
	inline bool isAcceptedRTCacheVersion(uint16_t version, bool isDayCache)
	{
		return version == 1 || (isDayCache && version == BLOCK_VERSION_DAY_T0);
	}

	inline void normalize(wtp::WTSBarStruct& bar)
	{
		bar.time = 0;
	}

	inline void normalize(std::vector<wtp::WTSBarStruct>& bars)
	{
		for (wtp::WTSBarStruct& bar : bars)
			bar.time = 0;
	}

	//buffer 为连续的 WTSBarStruct 字节（WtBtDtReaderAD::read_raw_bars 的输出）
	inline void normalizeRaw(std::string& buffer)
	{
		const std::size_t n = buffer.size() / sizeof(wtp::WTSBarStruct);
		for (std::size_t i = 0; i < n; ++i)
		{
			const uint64_t zero = 0;
			std::memcpy(&buffer[i * sizeof(wtp::WTSBarStruct) + offsetof(wtp::WTSBarStruct, time)], &zero, sizeof(zero));
		}
	}
}
