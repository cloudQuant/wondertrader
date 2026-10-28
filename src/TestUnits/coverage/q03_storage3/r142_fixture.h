// r142 / 共用夹具：逐笔历史块的「不可压缩」样本
//
// CMP_V2 历史逐笔在 WtDataStorage / WtDataStorageAD 两个读取器里都是
// `resize(sizeof(HisXxxBlock)) + append(uncompressed)` 之后才回写头部版本号
// （BUG-R132-001 同型路径）。只要压缩后长度小于解压后记录长度，append 就会
// 重新分配缓冲区并让旧的头部指针悬垂。本夹具用确定性伪随机字节填满整条记录，
// 只保留比较器真正用到的 action_date / action_time，让 zstd 的压缩结果不小于
// 原始载荷，从而稳定走「不重分配」的安全分支。
//
// 只依赖 Includes 中的公开结构体与 q03 沙箱；不触碰私有成员、不加访问宏。
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>

#include "q03_storage3_fixture.h"

namespace r142 {

inline void fill_noise(void* data, std::size_t size, uint32_t seed)
{
	std::mt19937 gen(seed);
	unsigned char* p = (unsigned char*)data;
	for (std::size_t i = 0; i < size; i++)
		p[i] = (unsigned char)(gen() & 0xFFu);
}

// 除 action_date / action_time 外全部为伪随机字节：切片只按这两个字段排序，
// 其余字段（含 exchg/code/trading_date）不参与比较，也不会被打印。
inline WTSOrdQueStruct noisy_ordque(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	(void)tdate;
	WTSOrdQueStruct item;
	fill_noise(&item, sizeof(item), seq * 2654435761u + 17u);
	item.action_date = adate;
	item.action_time = atime;
	return item;
}

inline WTSOrdDtlStruct noisy_orddtl(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	(void)tdate;
	WTSOrdDtlStruct item;
	fill_noise(&item, sizeof(item), seq * 2246822519u + 29u);
	item.action_date = adate;
	item.action_time = atime;
	return item;
}

inline WTSTransStruct noisy_trans(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	(void)tdate;
	WTSTransStruct item;
	fill_noise(&item, sizeof(item), seq * 3266489917u + 43u);
	item.action_date = adate;
	item.action_time = atime;
	return item;
}

} // namespace r142
