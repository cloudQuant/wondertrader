// L04 / WtDataStorageAD：LMDB 键编码（LMDBKeys.h）单元测试
// 覆盖 reverseEndian、LMDBHftKey/LMDBBarKey 的二进制布局与大端有序性。
// 全部为纯内存计算，不触盘、不依赖 LMDB 环境，确定性。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Includes/WTSMarcos.h"
#include "WtDataStorageAD/LMDBKeys.h"

namespace {

template <typename KeyType>
std::string key_bytes(const KeyType& key)
{
	return std::string((const char*)&key, sizeof(KeyType));
}

// 把键里的原始（大端存储）字段还原成宿主字节序的整数
template <typename FieldType>
FieldType read_field(const void* key, std::size_t byte_offset)
{
	FieldType be = 0;
	std::memcpy((void*)&be, (const char*)key + byte_offset, sizeof(FieldType));
	return reverseEndian(be);
}

}

//////////////////////////////////////////////////////////////////////////
// reverseEndian
//////////////////////////////////////////////////////////////////////////

TEST(L04LmdbKeys, ReverseEndian16KnownVectors)
{
	EXPECT_EQ(reverseEndian((uint16_t)0x0000), (uint16_t)0x0000);
	EXPECT_EQ(reverseEndian((uint16_t)0x0001), (uint16_t)0x0100);
	EXPECT_EQ(reverseEndian((uint16_t)0x1234), (uint16_t)0x3412);
	EXPECT_EQ(reverseEndian((uint16_t)0xFFFF), (uint16_t)0xFFFF);
	EXPECT_EQ(reverseEndian((uint16_t)0x8000), (uint16_t)0x0080);
}

TEST(L04LmdbKeys, ReverseEndian32KnownVectors)
{
	EXPECT_EQ(reverseEndian((uint32_t)0x00000000), (uint32_t)0x00000000);
	EXPECT_EQ(reverseEndian((uint32_t)0x00000001), (uint32_t)0x01000000);
	EXPECT_EQ(reverseEndian((uint32_t)0x12345678), (uint32_t)0x78563412);
	EXPECT_EQ(reverseEndian((uint32_t)0xFFFFFFFF), (uint32_t)0xFFFFFFFF);
	EXPECT_EQ(reverseEndian((uint32_t)0x000000FF), (uint32_t)0xFF000000);
}

TEST(L04LmdbKeys, ReverseEndianIsInvolution)
{
	const uint32_t samples[] = { 0u, 1u, 255u, 93000500u, 20240102u, 0x7FFFFFFFu, 0xFFFFFFFFu };
	for (uint32_t v : samples)
		EXPECT_EQ(reverseEndian(reverseEndian(v)), v) << "value " << v;

	const uint16_t samples16[] = { 0u, 1u, 0x7Fu, 0x1234u, 0xFFFFu };
	for (uint16_t v : samples16)
		EXPECT_EQ(reverseEndian(reverseEndian(v)), v) << "value " << (uint32_t)v;
}

TEST(L04LmdbKeys, ReverseEndianBytesMatchMemoryOrder)
{
	// 反转后的字节序应等价于把原值按大端写入内存
	uint32_t src = 20240102u;
	uint32_t be = reverseEndian(src);
	unsigned char raw[4] = { 0, 0, 0, 0 };
	std::memcpy(raw, (const void*)&be, 4);
	EXPECT_EQ(raw[0], (unsigned char)((src >> 24) & 0xFF));
	EXPECT_EQ(raw[1], (unsigned char)((src >> 16) & 0xFF));
	EXPECT_EQ(raw[2], (unsigned char)((src >> 8) & 0xFF));
	EXPECT_EQ(raw[3], (unsigned char)(src & 0xFF));
}

//////////////////////////////////////////////////////////////////////////
// LMDBBarKey 布局
//////////////////////////////////////////////////////////////////////////

TEST(L04LmdbKeys, BarKeyPackedLayout)
{
	LMDBBarKey key("SHFE", "rb2401", 20240102u);
	EXPECT_EQ(sizeof(LMDBBarKey), (std::size_t)MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH + 4);

	// _exchg 在偏移 0
	EXPECT_EQ((const char*)&key - (const char*)&key._exchg[0], (std::ptrdiff_t)0);
	// _code 紧跟 _exchg
	EXPECT_EQ((const char*)&key._code[0] - (const char*)&key._exchg[0], (std::ptrdiff_t)MAX_EXCHANGE_LENGTH);
	// _bartime 紧跟 _code
	EXPECT_EQ((const char*)&key._bartime - (const char*)&key._code[0], (std::ptrdiff_t)MAX_INSTRUMENT_LENGTH);

	EXPECT_STREQ(key._exchg, "SHFE");
	EXPECT_STREQ(key._code, "rb2401");
}

TEST(L04LmdbKeys, BarKeyTimeStoredBigEndian)
{
	const uint32_t raw = 20240102u;
	LMDBBarKey key("SHFE", "rb2401", raw);
	EXPECT_EQ(key._bartime, reverseEndian(raw));
	EXPECT_NE(key._bartime, raw);
	EXPECT_EQ(read_field<uint32_t>((const void*)&key, MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH), raw);
}

TEST(L04LmdbKeys, BarKeyPaddingZeroed)
{
	LMDBBarKey key("CFFEX", "IF2401", 100u);
	const unsigned char* p = (const unsigned char*)&key;
	// "CFFEX" 5 字节之后到 _code 之前必须全部为 0
	for (int i = 5; i < MAX_EXCHANGE_LENGTH; i++)
		EXPECT_EQ(p[i], 0);
	// "IF2401" 6 字节之后到 _bartime 之前必须全部为 0
	for (int i = MAX_EXCHANGE_LENGTH + 6; i < MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH; i++)
		EXPECT_EQ(p[i], 0);
}

TEST(L04LmdbKeys, BarKeyStringViewKeepsFullLength)
{
	// 存储侧用 std::string((const char*)&key, sizeof(key)) 作为键，内嵌 0 不能截断
	LMDBBarKey key("SHFE", "rb2401", 0u);
	std::string view = key_bytes(key);
	EXPECT_EQ(view.size(), sizeof(LMDBBarKey));
	EXPECT_EQ(view.compare(0, 4, std::string("SHFE")), 0);
}

//////////////////////////////////////////////////////////////////////////
// LMDBHftKey 布局
//////////////////////////////////////////////////////////////////////////

TEST(L04LmdbKeys, HftKeyPackedLayout)
{
	LMDBHftKey key("SZSE", "000001", 20240102u, 93000500u);
	EXPECT_EQ(sizeof(LMDBHftKey), (std::size_t)MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH + 8);
	EXPECT_EQ((const char*)&key._time - (const char*)&key._date, (std::ptrdiff_t)4);
	EXPECT_EQ((const char*)&key._date - (const char*)&key._code[0], (std::ptrdiff_t)MAX_INSTRUMENT_LENGTH);

	EXPECT_STREQ(key._exchg, "SZSE");
	EXPECT_STREQ(key._code, "000001");
	EXPECT_EQ(key._date, reverseEndian(20240102u));
	EXPECT_EQ(key._time, reverseEndian(93000500u));
}

TEST(L04LmdbKeys, HftKeyDateSplitRoundTrip)
{
	// WtDataReaderAD 用 etime/1000000000 与 etime%1000000000 拆出 date/time
	const uint64_t etime = 20240102093000500ull;
	const uint32_t date = (uint32_t)(etime / 1000000000ull);
	const uint32_t time = (uint32_t)(etime % 1000000000ull);
	EXPECT_EQ(date, 20240102u);
	EXPECT_EQ(time, 93000500u);

	LMDBHftKey key("SHFE", "rb2401", date, time);
	EXPECT_EQ(read_field<uint32_t>((const void*)&key, MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH), date);
	EXPECT_EQ(read_field<uint32_t>((const void*)&key, MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH + 4), time);
}

//////////////////////////////////////////////////////////////////////////
// LMDB 依赖的字典序 == 数值序
//////////////////////////////////////////////////////////////////////////

TEST(L04LmdbKeys, BarKeyLexicographicOrderMatchesTimeOrder)
{
	const uint32_t times[] = { 0u, 1u, 93000u, 150000u, 20240102u, 0xFFFFFFFEu, 0xFFFFFFFFu };
	std::vector<std::string> keys;
	keys.reserve(sizeof(times) / sizeof(times[0]));
	for (uint32_t t : times)
		keys.emplace_back(key_bytes(LMDBBarKey("SHFE", "rb2401", t)));

	for (std::size_t i = 1; i < keys.size(); i++)
	{
		EXPECT_LT(memcmp(keys[i - 1].data(), keys[i].data(), sizeof(LMDBBarKey)), 0)
			<< "barkey " << times[i - 1] << " should sort before " << times[i];
	}
}

TEST(L04LmdbKeys, HftKeyLexicographicOrderMatchesDateThenTime)
{
	struct Item { uint32_t date; uint32_t time; };
	const Item items[] = {
		{20240102u, 93000000u}, {20240102u, 93000001u}, {20240102u, 150000000u},
		{20240103u, 0u}, {20240103u, 93000000u}, {20240104u, 0xFFFFFFFFu}
	};
	std::vector<std::string> keys;
	for (Item it : items)
		keys.emplace_back(key_bytes(LMDBHftKey("SHFE", "rb2401", it.date, it.time)));

	for (std::size_t i = 1; i < keys.size(); i++)
	{
		EXPECT_LT(memcmp(keys[i - 1].data(), keys[i].data(), sizeof(LMDBHftKey)), 0)
			<< "item " << i - 1 << " should sort before " << i;
	}
}

TEST(L04LmdbKeys, FullRangeSentinelsBracketEveryKey)
{
	// AD 读取侧惯用 (0, 0xffffffff) 作为区间边界
	const uint32_t probes[] = { 0u, 1u, 20240102u, 93000500u, 0x7FFFFFFFu, 0xFFFFFFFEu };
	LMDBBarKey lo("SHFE", "rb2401", 0);
	LMDBBarKey hi("SHFE", "rb2401", 0xffffffff);
	for (uint32_t p : probes)
	{
		LMDBBarKey mid("SHFE", "rb2401", p);
		EXPECT_LE(memcmp((const void*)&lo, (const void*)&mid, sizeof(LMDBBarKey)), 0) << "probe " << p;
		EXPECT_LE(memcmp((const void*)&mid, (const void*)&hi, sizeof(LMDBBarKey)), 0) << "probe " << p;
	}

	LMDBHftKey tlo("SHFE", "rb2401", 0, 0);
	LMDBHftKey thi("SHFE", "rb2401", 0xffffffff, 240000000u);
	const uint32_t dates[] = { 0u, 20240102u, 20241231u, 0xFFFFFFFFu };
	const uint32_t times[] = { 0u, 93000500u, 240000000u };
	for (uint32_t d : dates)
	{
		for (uint32_t t : times)
		{
			LMDBHftKey m("SHFE", "rb2401", d, t);
			if (d == 0xFFFFFFFFu)
				continue;	// 越界哨兵本身不参与比较
			EXPECT_LE(memcmp((const void*)&tlo, (const void*)&m, sizeof(LMDBHftKey)), 0);
			if (t <= 240000000u)
				EXPECT_LE(memcmp((const void*)&m, (const void*)&thi, sizeof(LMDBHftKey)), 0);
		}
	}
}

TEST(L04LmdbKeys, SameCodeDifferentExchgSortsByExchg)
{
	LMDBBarKey a("CFFEX", "IF2401", 100u);
	LMDBBarKey b("SHFE", "rb2401", 100u);
	LMDBBarKey c("SZSE", "000001", 100u);
	EXPECT_LT(memcmp((const void*)&a, (const void*)&b, sizeof(LMDBBarKey)), 0);
	EXPECT_LT(memcmp((const void*)&b, (const void*)&c, sizeof(LMDBBarKey)), 0);
}

TEST(L04LmdbKeys, ExchgPrefixDoesNotCollideAcrossContracts)
{
	// "SHFE" + code 前缀相同的两个合约必须产生不同键
	LMDBBarKey k1("SHFE", "rb2401", 20240102u);
	LMDBBarKey k2("SHFE", "rb2402", 20240102u);
	EXPECT_NE(memcmp((const void*)&k1, (const void*)&k2, sizeof(LMDBBarKey)), 0);
	// 空交易所/空代码也不应相等（仅 bartime 参与区分）
	LMDBBarKey e1("", "", 1u);
	LMDBBarKey e2("", "", 2u);
	EXPECT_NE(memcmp((const void*)&e1, (const void*)&e2, sizeof(LMDBBarKey)), 0);
}

TEST(L04LmdbKeys, MaxLengthFieldsFitExactly)
{
	// MAX_EXCHANGE_LENGTH-1 / MAX_INSTRUMENT_LENGTH-1 个字符刚好放下且以 0 结尾
	std::string exchg(MAX_EXCHANGE_LENGTH - 1, 'A');
	std::string code(MAX_INSTRUMENT_LENGTH - 1, 'B');
	LMDBBarKey key(exchg.c_str(), code.c_str(), 12345u);
	EXPECT_EQ(strlen(key._exchg), (std::size_t)MAX_EXCHANGE_LENGTH - 1);
	EXPECT_EQ(strlen(key._code), (std::size_t)MAX_INSTRUMENT_LENGTH - 1);
	EXPECT_EQ(key._bartime, reverseEndian(12345u));

	LMDBHftKey hkey(exchg.c_str(), code.c_str(), 20240102u, 93000500u);
	EXPECT_EQ(strlen(hkey._exchg), (std::size_t)MAX_EXCHANGE_LENGTH - 1);
	EXPECT_EQ(strlen(hkey._code), (std::size_t)MAX_INSTRUMENT_LENGTH - 1);
}

TEST(L04LmdbKeys, KeySizeMatchesStorageStringSize)
{
	// WtDataStorageAD 各处都以 sizeof(key) 构造 std::string，尺寸必须与库内键宽一致
	EXPECT_EQ(key_bytes(LMDBBarKey("SHFE", "rb2401", 1u)).size(), sizeof(LMDBBarKey));
	EXPECT_EQ(key_bytes(LMDBHftKey("SHFE", "rb2401", 1u, 1u)).size(), sizeof(LMDBHftKey));
	EXPECT_EQ(sizeof(LMDBBarKey) + 4, sizeof(LMDBHftKey));
}
