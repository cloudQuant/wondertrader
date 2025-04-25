/**
 * @file FasterDefs.h
 * @brief 性能优化相关定义
 * @details 定义了WonderTrader框架中使用的高性能数据结构，如哈希表和哈希集合
 * @version 1.0
 * @author Wesley
 * @date 2023-08-15
 */
#pragma once
#include <string.h>
#include "WTSMarcos.h"

/**
 * @brief 引入robin_map和robin_set
 * @details 引入tessil库中的robin_map和robin_set实现，这是一种高效的哈希表和集合实现
 * @see https://github.com/Tessil/robin-map
 */
#include "../FasterLibs/tsl/robin_map.h"
#include "../FasterLibs/tsl/robin_set.h"

/**
 * @brief 引入unordered_dense哈希表和集合
 * @details 引入ankerl库中的unordered_dense map和set实现，这是另一种高效的哈希表和集合实现
 * @see https://github.com/martinus/unordered_dense
 */
#include "../FasterLibs/ankerl/unordered_dense.h"

/**
 * @brief Robin Map在大数据量时的限制
 * @author Wesley @ 2023.08.15
 * @details 很遗憾，robin_map搭配 std::string在数据量大的时候（经测试在13106条数据，不同测试机可能具体数值不同）
 * 会出现bad allocate的异常
 * 我猜测是 std::string无法像string那样自动优化
 * 所以数据量大的时候，就会占用非常大的内存，当运行环境内存较小时，就会出现异常
 * 所以这次把LongKey和LongKey都注释掉，改成std::string
 */

/**
 * @brief Ankerl与Robin的性能对比
 * @author Wesley @ 2023.08.16
 * @details ankerl写入速度比robin好很多，大概快1/3，尤其数据量在40w以内的时候
 * 但是robin的读取速度比robin好，不过到了30w条数据以内，差别就不大
 * 按照wondertrader的场景，还是ankerl要好很多
 * 具体可以参考以下页面的性能对比
 * @see https://martin.ankerl.com/2022/08/27/hashmap-bench-01/#benchmark-results-table
 */

NS_WTP_BEGIN

/**
 * @brief 字符串哈希函数类
 * @details 定义了一个用于计算字符串哈希值的函数对象，采用了BKDR哈希算法
 * @see Brian Kernighan和Dennis Ritchie提出的哈希算法，被称为BKDR Hash
 */
struct string_hash
{
	/**
	 * @brief 计算字符串的哈希值
	 * @param key 输入字符串
	 * @return 哈希值
	 * @details 实现了BKDR哈希算法，该算法在字符串哈希中有较低的冲突率
	 */
	std::size_t operator()(const std::string& key) const
	{
		size_t seed = 131; // 31 131 1313 13131 131313 etc.. 可选的哈希乘数
		size_t hash = 0;

		char* str = (char*)key.c_str();
		while (*str)  // 逐字符计算哈希值
		{
			hash = hash * seed + (*str++);  // 累加当前字符的哈希值
		}

		return (hash & 0x7FFFFFFF);  // 确保哈希值为正数
	}
};

/**
 * @brief 通用的高性能哈希表模板类
 * @tparam Key 键类型
 * @tparam T 值类型
 * @details 基于robin_map的通用哈希表实现，为非字符串类型的键提供高效的哈希表
 */
template<class Key, class T>
class fastest_hashmap : public tsl::robin_map<Key, T>
{
public:
	/// @brief robin_map容器类型别名
	typedef tsl::robin_map<Key, T>	Container;
	
	/// @brief 默认构造函数
	fastest_hashmap():Container(){}
};

/**
 * @brief 字符串键的高性能哈希表类
 * @tparam T 值类型
 * @details 针对std::string键的fastest_hashmap特化版本，使用自定义的string_hash函数
 *          以减少哈希冲突并提高性能
 */
template<class T>
class fastest_hashmap<std::string, T> : public tsl::robin_map<std::string, T, string_hash>
{
public:
	/// @brief 为字符串键使用自定义哈希函数的robin_map容器类型
	typedef tsl::robin_map<std::string, T, string_hash>	Container;
	
	/// @brief 默认构造函数
	fastest_hashmap() :Container() {}
};

/**
 * @brief 通用的高性能哈希集合模板类
 * @tparam Key 元素类型
 * @details 基于robin_set的通用哈希集合实现，为非字符串类型的元素提供高效的哈希集合
 */
template<class Key>
class fastest_hashset : public tsl::robin_set<Key>
{
public:
	/// @brief robin_set容器类型别名
	typedef tsl::robin_set<Key>	Container;
	
	/// @brief 默认构造函数
	fastest_hashset() :Container() {}
};

/**
 * @brief 字符串元素的高性能哈希集合类
 * @details 针对std::string元素的fastest_hashset完全特化版本，使用自定义的string_hash函数
 *          以减少哈希冲突并提高性能
 */
template<>
class fastest_hashset<std::string> : public tsl::robin_set<std::string, string_hash>
{
public:
	/// @brief 为字符串元素使用自定义哈希函数的robin_set容器类型
	typedef tsl::robin_set<std::string, string_hash>	Container;
	
	/// @brief 默认构造函数
	fastest_hashset() :Container() {}
};

/**
 * @brief 代码集合类型别名
 * @details 为字符串哈希集合定义一个类型别名，用于存储合约代码等字符串集合
 */
typedef fastest_hashset<std::string> CodeSet;

//////////////////////////////////////////////////////////////////////////
//下面使用unordered_dense实现的更高效哈希容器

/**
 * @brief WonderTrader通用高性能哈希表模板类
 * @tparam Key 键类型
 * @tparam T 值类型
 * @tparam Hash 哈希函数类型，默认使用std::hash
 * @details 基于ankerl库的unordered_dense::map实现，提供比STL和tsl::robin_map更高效的哈希表
 *          特别是在写入操作方面有更好的性能
 */
template<class Key, class T, class Hash = std::hash<Key>>
class wt_hashmap : public ankerl::unordered_dense::map<Key, T, Hash>
{
public:
	/// @brief unordered_dense::map容器类型别名
	typedef ankerl::unordered_dense::map<Key, T, Hash>	Container;
	
	/// @brief 默认构造函数
	wt_hashmap() :Container() {}
};

/**
 * @brief 字符串键的WonderTrader高性能哈希表类
 * @tparam T 值类型
 * @details 针对std::string键的wt_hashmap特化版本，使用自定义的string_hash函数
 *          基于ankerl库的unordered_dense::map实现，在字符串键的哈希表中有更高的性能
 */
template<class T>
class wt_hashmap<std::string, T, string_hash> : public ankerl::unordered_dense::map<std::string, T, string_hash>
{
public:
	/// @brief 为字符串键使用自定义哈希函数的unordered_dense::map容器类型
	typedef ankerl::unordered_dense::map<std::string, T, string_hash>	Container;
	
	/// @brief 默认构造函数
	wt_hashmap() :Container() {}
};

/**
 * @brief WonderTrader通用高性能哈希集合模板类
 * @tparam Key 元素类型
 * @tparam Hash 哈希函数类型，默认使用std::hash
 * @details 基于ankerl库的unordered_dense::set实现，提供比STL和tsl::robin_set更高效的哈希集合
 *          特别是在写入操作方面有更好的性能
 */
template<class Key, class Hash = std::hash<Key>>
class wt_hashset : public ankerl::unordered_dense::set<Key, Hash>
{
public:
	/// @brief unordered_dense::set容器类型别名
	typedef ankerl::unordered_dense::set<Key, Hash>	Container;
	
	/// @brief 默认构造函数
	wt_hashset() :Container() {}
};

/**
 * @brief 字符串元素的WonderTrader高性能哈希集合类
 * @details 针对std::string元素的wt_hashset完全特化版本，使用自定义的string_hash函数
 *          基于ankerl库的unordered_dense::set实现，在字符串元素的哈希集合中有更高的性能
 */
template<>
class wt_hashset<std::string, string_hash> : public ankerl::unordered_dense::set<std::string, string_hash>
{
public:
	/// @brief 为字符串元素使用自定义哈希函数的unordered_dense::set容器类型
	typedef ankerl::unordered_dense::set<std::string, string_hash>	Container;
	
	/// @brief 默认构造函数
	wt_hashset() :Container() {}
};

/**
 * @brief 结束 WonderTrader 命名空间
 */
NS_WTP_END
