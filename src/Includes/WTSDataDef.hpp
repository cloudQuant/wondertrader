/*!
 * \file WTSDataDef.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader行情数据定义文件，包括tick、bar、orderqueue、orderdetail、transaction等数据
 * 
 * 本文件定义了WonderTrader框架中所有基础行情数据的数据结构和容器类
 * 是整个框架的核心数据定义之一，理解这些数据结构对理解策略开发至关重要
 */
#pragma once
#include <stdlib.h>
#include <vector>
#include <deque>
#include <string.h>
#include <chrono>

#include "WTSObject.hpp"

#include "WTSTypes.h"
#include "WTSMarcos.h"
#include "WTSStruct.h"
#include "WTSCollection.hpp"

using namespace std;

#pragma warning(disable:4267)


NS_WTP_BEGIN
class WTSContractInfo; // 合约信息类前向声明

/**
 * @brief 数值数组类
 * 
 * 数值数组的内部封装，负责存储和管理一组double类型的数值
 * 采用std::vector实现，提供了便捷的访问和操作方法
 * 主要用于存储指标计算结果等数值序列
 * 支持负索引访问（如-1表示最后一个元素）
 */
class WTSValueArray : public WTSObject
{
protected:
	vector<double>	m_vecData;  // 内部数据存储容器，使用vector存储double值

public:
	/**
	 * @brief 创建一个数值数组对象
	 * 
	 * 工厂方法，用于创建WTSValueArray对象
	 * WonderTrader中常用工厂模式创建对象，而不是直接使用new
	 * 
	 * @return WTSValueArray* 返回创建的数值数组对象指针
	 */
	static WTSValueArray* create()
	{
		WTSValueArray* pRet = new WTSValueArray;
		pRet->m_vecData.clear();  // 初始化数据容器
		return pRet;
	}

	/**
	 * @brief 读取数组的长度
	 * 
	 * @return uint32_t 返回数组中元素的数量
	 */
	inline uint32_t	size() const{ return m_vecData.size(); }
	
	/**
	 * @brief 判断数组是否为空
	 * 
	 * @return bool 如果数组为空返回true，否则返回false
	 */
	inline bool		empty() const{ return m_vecData.empty(); }

	/**
	 * @brief 读取指定位置的数据
	 * 
	 * 安全地访问数组指定位置的元素，支持负索引
	 * 如果索引超出范围，则返回INVALID_DOUBLE（无效值常量）
	 * 
	 * @param idx 索引位置，支持负数索引（如-1表示最后一个元素）
	 * @return double 返回指定位置的数值，超出范围则返回INVALID_DOUBLE
	 */
	inline double		at(uint32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= m_vecData.size())  // 检查索引是否有效
			return INVALID_DOUBLE;  // 返回无效值常量

		return m_vecData[idx];  // 返回指定位置的值
	}

	/**
	 * @brief 转换索引值
	 * 
	 * 将可能为负的索引转换为正索引
	 * 例如，-1表示最后一个元素，-2表示倒数第二个元素，以此类推
	 * 这种方式在指标计算中非常有用，方便引用最近的数据
	 * 
	 * @param idx 原始索引，可能为负数
	 * @return int32_t 转换后的非负索引
	 */
	inline int32_t		translateIdx(int32_t idx) const
	{
		if(idx < 0)  // 如果是负索引
		{
			return m_vecData.size()+idx;  // 将其转换为从末尾计算的正索引
		}

		return idx;  // 如果已经是非负索引，则直接返回
	}

	/**
	 * @brief 找到指定范围内的最大值
	 * 
	 * 在数组的指定区间[head, tail]内查找最大值
	 * 区间支持负索引（如-1表示最后一个元素）
	 * 如果区间超出数组范围或无有效值，则返回INVALID_DOUBLE
	 * 
	 * @param head 区间起始位置
	 * @param tail 区间结束位置
	 * @param isAbs 是否使用绝对值比较，默认为false
	 * @return double 返回区间内的最大值，无有效值则返回INVALID_DOUBLE
	 */
	double		maxvalue(int32_t head, int32_t tail, bool isAbs = false) const
	{
		head = translateIdx(head);
		tail = translateIdx(tail);

		uint32_t begin = min(head, tail);
		uint32_t end = max(head, tail);

		if(begin <0 || begin >= m_vecData.size() || end < 0 || end > m_vecData.size())
			return INVALID_DOUBLE;

		double maxValue = INVALID_DOUBLE;
		for(uint32_t i = begin; i <= end; i++)
		{
			if(m_vecData[i] == INVALID_DOUBLE)
				continue;

			if(maxValue == INVALID_DOUBLE)
				maxValue = isAbs?abs(m_vecData[i]):m_vecData[i];
			else
				maxValue = max(maxValue, isAbs?abs(m_vecData[i]):m_vecData[i]);
		}

		//if (maxValue == INVALID_DOUBLE)
		//	maxValue = 0.0;

		return maxValue;
	}

	/**
	 * @brief 找到指定范围内的最小值
	 * 
	 * 在数组的指定区间[head, tail]内查找最小值
	 * 区间支持负索引（如-1表示最后一个元素）
	 * 如果区间超出数组范围或无有效值，则返回INVALID_DOUBLE
	 * 
	 * @param head 区间起始位置
	 * @param tail 区间结束位置
	 * @param isAbs 是否使用绝对值比较，默认为false
	 * @return double 返回区间内的最小值，无有效值则返回INVALID_DOUBLE
	 */
	double		minvalue(int32_t head, int32_t tail, bool isAbs = false) const
	{
		head = translateIdx(head);
		tail = translateIdx(tail);

		uint32_t begin = min(head, tail);
		uint32_t end = max(head, tail);

		if(begin <0 || begin >= m_vecData.size() || end < 0 || end > m_vecData.size())
			return INVALID_DOUBLE;

		double minValue = INVALID_DOUBLE;
		for(uint32_t i = begin; i <= end; i++)
		{
			if (m_vecData[i] == INVALID_DOUBLE)
				continue;

			if(minValue == INVALID_DOUBLE)
				minValue = isAbs?abs(m_vecData[i]):m_vecData[i];
			else
				minValue = min(minValue, isAbs?abs(m_vecData[i]):m_vecData[i]);
		}

		//if (minValue == INVALID_DOUBLE)
		//	minValue = 0.0;

		return minValue;
	}

	/**
	 * @brief 在数组末尾添加数据
	 * 
	 * 将一个新的double值添加到数组末尾
	 * 使用emplace_back而不是push_back，效率更高
	 * 
	 * @param val 要添加的值
	 */
	inline void		append(double val)
	{
		m_vecData.emplace_back(val);  // 使用emplace_back提高效率
	}

	/**
	 * @brief 设置指定位置的数据
	 * 
	 * 修改数组中指定索引位置的值
	 * 如果索引超出范围，则不执行任何操作
	 * 
	 * @param idx 要设置的索引位置
	 * @param val 要设置的新值
	 */
	inline void		set(uint32_t idx, double val)
	{
		if(idx < 0 || idx >= m_vecData.size())  // 检查索引是否有效
			return;  // 无效索引直接返回，不做任何操作

		m_vecData[idx] = val;  // 设置指定位置的值
	}

	/**
	 * @brief 重新分配数组大小，并设置默认值
	 * 
	 * 调整数组的大小
	 * 如果新的大小大于当前大小，扩展部分的元素将被初始化为指定的值
	 * 
	 * @param uSize 新的数组大小
	 * @param val 扩展部分元素的默认值，默认为INVALID_DOUBLE
	 */
	inline void		resize(uint32_t uSize, double val = INVALID_DOUBLE)
	{
		m_vecData.resize(uSize, val);  // 调整容器大小并设置默认值
	}

	/**
	 * @brief 重载操作符[]（非常量版本）
	 * 
	 * 允许通过数组下标语法直接访问和修改数组元素
	 * 注意：这个操作符不检查索引是否越界，不安全
	 * 如果需要安全访问，应使用at()方法
	 * 
	 * @param idx 索引位置
	 * @return double& 返回对应位置元素的引用，可以修改
	 */
	inline double&		operator[](uint32_t idx)
	{
		return m_vecData[idx];  // 返回引用，可以修改值
	}

	/**
	 * @brief 重载操作符[]（常量版本）
	 * 
	 * 允许在常量对象上通过数组下标语法访问元素（只读）
	 * 注意：这个操作符不检查索引是否越界，不安全
	 * 
	 * @param idx 索引位置
	 * @return double 返回对应位置元素的值，只读
	 */
	inline double		operator[](uint32_t idx) const
	{
		return m_vecData[idx];  // 返回值，不可修改
	}

	/**
	 * @brief 获取内部数据容器的引用
	 * 
	 * 直接返回内部vector容器的引用，提供对原始数据的访问
	 * 谨慎使用，因为它暴露了内部实现
	 * 
	 * @return std::vector<double>& 返回内部数据容器的引用
	 */
	inline std::vector<double>& getDataRef()
	{
		return m_vecData;  // 返回内部容器的引用
	}
};

/**
 * @brief K线数据切片类
 * 
 * K线数据切片用于访问一段连续的K线数据
 * 这个类比较特殊，因为需要拼接当日和历史的K线数据
 * 采用块存储的方式，可以包含多个不连续的内存块
 * 维护多个内存块的指针和大小，在访问时进行计算和定位
 * 是策略开发中获取历史K线数据的重要容器
 */
class WTSKlineSlice : public WTSObject
{
private:
	char			_code[MAX_INSTRUMENT_LENGTH];  // 合约代码
	WTSKlinePeriod	_period;  // K线周期（如1分钟、5分钟、日线等）
	uint32_t		_times;  // 周期倍数，例如_period为分钟，_times为5表示5分钟K线
	typedef std::pair<WTSBarStruct*, uint32_t> BarBlock;  // K线数据块定义，first为数据指针，second为数据数量
	std::vector<BarBlock> _blocks;  // 存储多个K线数据块的容器
	uint32_t		_count;  // 总的K线数量

protected:
	/**
	 * @brief 构造函数
	 * 
	 * 默认构造函数，初始化K线周期为1分钟，倍数为1，总数为0
	 * 受保护的构造函数，不允许直接创建实例，应使用工厂方法create
	 */
	WTSKlineSlice()
		: _period(KP_Minute1)  // 默认周期为1分钟
		, _times(1)  // 默认倍数为1
		, _count(0)  // 默认K线总数为0
	{

	}

	/**
	 * @brief 转换索引值
	 * 
	 * 将可能为负的索引转换为非负索引
	 * 例如，-1表示最后一个K线，-2表示倒数第二个K线
	 * 
	 * @param idx 原始索引，可能为负数
	 * @return int32_t 转换后的非负索引
	 */
	inline int32_t		translateIdx(int32_t idx) const
	{
		int32_t totalCnt = _count;  // 获取总K线数量
		if (idx < 0)  // 如果是负索引
		{
			return max(0, totalCnt + idx);  // 转换为从末尾计算的正索引，确保不小于0
		}

		return idx;  // 如果已经是非负索引，则直接返回
	}


public:
	/**
	 * @brief 创建K线切片对象的工厂方法
	 * 
	 * 创建一个新的K线切片对象，可选择性地添加初始数据块
	 * 遵循工厂模式，是创建WTSKlineSlice对象的标准方式
	 * 
	 * @param code 合约代码
	 * @param period K线周期类型（如分钟、日线等）
	 * @param times 周期倍数（如5分钟K线的倍数为5）
	 * @param bars 初始K线数据指针，可为NULL
	 * @param count 初始K线数据数量
	 * @return WTSKlineSlice* 返回创建的K线切片对象指针
	 */
	static WTSKlineSlice* create(const char* code, WTSKlinePeriod period, uint32_t times, WTSBarStruct* bars = NULL, int32_t count = 0)
	{
		WTSKlineSlice *pRet = new WTSKlineSlice;  // 创建新的K线切片对象
		wt_strcpy(pRet->_code, code);  // 设置合约代码
		pRet->_period = period;  // 设置K线周期
		pRet->_times = times;  // 设置周期倍数
		if(bars)  // 如果提供了初始数据
			pRet->_blocks.emplace_back(BarBlock(bars, count));  // 添加初始数据块
		pRet->_count = count;  // 设置总K线数量

		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 追加K线数据块
	 * 
	 * 向K线切片中添加一个新的K线数据块
	 * 这个功能使得K线切片可以管理多个不连续的内存块，提高内存使用效率
	 * 
	 * @param bars 要添加的K线数据指针
	 * @param count 要添加的K线数据数量
	 * @return bool 添加成功返回true，失败返回false
	 */
	inline bool appendBlock(WTSBarStruct* bars, uint32_t count)
	{
		if (bars == NULL || count == 0)  // 检查参数有效性
			return false;

		_count += count;  // 增加总K线数量
		_blocks.emplace_back(BarBlock(bars, count));  // 添加新的数据块
		return true;
	}

	inline std::size_t	get_block_counts() const
	{
		return _blocks.size();
	}

	/**
	 * @brief 获取指定数据块的地址
	 * 
	 * 返回指定索引的数据块的内存地址（指针）
	 * 
	 * @param blkIdx 数据块索引
	 * @return WTSBarStruct* 数据块的起始地址，无效索引返回NULL
	 */
	inline WTSBarStruct*	get_block_addr(std::size_t blkIdx)
	{
		if (blkIdx >= _blocks.size())  // 检查索引有效性
			return NULL;

		return _blocks[blkIdx].first;  // 返回数据块的指针
	}

	/**
	 * @brief 获取指定数据块的大小
	 * 
	 * 返回指定索引的数据块中包含的K线数量
	 * 
	 * @param blkIdx 数据块索引
	 * @return uint32_t 数据块中的K线数量，无效索引返回0
	 */
	inline uint32_t get_block_size(std::size_t blkIdx)
	{
		if (blkIdx >= _blocks.size())  // 检查索引有效性
			return 0;

		return _blocks[blkIdx].second;  // 返回数据块中的K线数量
	}

	/**
	 * @brief 获取指定位置的K线数据（非常量版本）
	 * 
	 * 根据索引获取对应的K线数据指针
	 * 这个方法会在多个数据块中定位指定索引的K线
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 要获取的K线索引，支持负索引
	 * @return WTSBarStruct* 指定位置的K线数据指针，无效索引返回NULL
	 */
	inline WTSBarStruct*	at(int32_t idx)
	{
		if (_count == 0)  // 如果没有K线数据
			return NULL;

		idx = translateIdx(idx);  // 转换索引，处理负索引情况
		do
		{
			for (auto& item : _blocks)  // 遍历所有数据块
			{
				if ((uint32_t)idx >= item.second)  // 如果索引超过当前块的大小
					idx -= item.second;  // 减去当前块的大小，准备查找下一个块
				else
					return item.first + idx;  // 找到目标块，返回对应位置的K线指针
			}
		} while (false);  // 这个do-while(false)结构允许使用break跳出

		return NULL;  // 如果没有找到匹配的K线，返回NULL
	}

	/**
	 * @brief 获取指定位置的K线数据（常量版本）
	 * 
	 * 常量对象版本的at方法，返回的是常量指针，不可修改K线数据
	 * 功能与非常量版本相同，但返回类型为const WTSBarStruct*
	 * 
	 * @param idx 要获取的K线索引，支持负索引
	 * @return const WTSBarStruct* 指定位置的K线数据常量指针，无效索引返回NULL
	 */
	inline const WTSBarStruct*	at(int32_t idx) const
	{
		if (_count == 0)  // 如果没有K线数据
			return NULL;

		idx = translateIdx(idx);  // 转换索引，处理负索引情况
		do
		{
			for (auto& item : _blocks)  // 遍历所有数据块
			{
				if ((uint32_t)idx >= item.second)  // 如果索引超过当前块的大小
					idx -= item.second;  // 减去当前块的大小，准备查找下一个块
				else
					return item.first + idx;  // 找到目标块，返回对应位置的K线常量指针
			}
		} while (false);  // 这个do-while(false)结构允许使用break跳出
		return NULL;  // 如果没有找到匹配的K线，返回NULL
	}


	/**
	 * @brief 查找指定范围内的最大价格
	 * 
	 * 在指定的K线区间内查找最高价格（high字段）
	 * 支持负索引，会将索引转换为实际位置
	 * 
	 * @param head 区间起始位置
	 * @param tail 区间结束位置
	 * @return double 返回区间内的最高价格
	 */
	double		maxprice(int32_t head, int32_t tail) const
	{
		head = translateIdx(head);  // 转换起始位置索引
		tail = translateIdx(tail);  // 转换结束位置索引

		int32_t begin = max(0,min(head, tail));  // 确保起始位置不小于0
		int32_t end = min(max(head, tail), size() - 1);  // 确保结束位置不超出范围

		double maxValue = this->at(begin)->high;  // 初始化最大值为起始位置的高点
		for (int32_t i = begin; i <= end; i++)  // 遍历区间内的所有K线
		{
			maxValue = max(maxValue, at(i)->high);  // 更新最大值
		}
		return maxValue;  // 返回找到的最大值
	}

	/**
	 * @brief 查找指定范围内的最小价格
	 * 
	 * 在指定的K线区间内查找最低价格（low字段）
	 * 支持负索引，会将索引转换为实际位置
	 * 
	 * @param head 区间起始位置
	 * @param tail 区间结束位置
	 * @return double 返回区间内的最低价格
	 */
	double		minprice(int32_t head, int32_t tail) const
	{
		head = translateIdx(head);  // 转换起始位置索引
		tail = translateIdx(tail);  // 转换结束位置索引

		int32_t begin = max(0, min(head, tail));  // 确保起始位置不小于0
		int32_t end = min(max(head, tail), size() - 1);  // 确保结束位置不超出范围

		double minValue = at(begin)->low;  // 初始化最小值为起始位置的低点
		for (int32_t i = begin; i <= end; i++)  // 遍历区间内的所有K线
		{
			minValue = min(minValue, at(i)->low);  // 更新最小值
		}

		return minValue;  // 返回找到的最小值
	}

	/**
	 * @brief 返回K线切片中的K线总数
	 * 
	 * 获取当前K线切片中包含的所有K线数量
	 * 
	 * @return int32_t K线总数
	 */
	inline int32_t	size() const{ return _count; }
	
	/**
	 * @brief 判断K线切片是否为空
	 * 
	 * 检查当前K线切片是否不包含任何K线数据
	 * 
	 * @return bool 如果没有K线数据返回true，否则返回false
	 */
	inline bool	empty() const{ return _count == 0; }

	/**
	 * @brief 获取K线切片的合约代码
	 * 
	 * 返回当前K线切片对应的标的合约代码
	 * 
	 * @return const char* 合约代码字符串
	 */
	inline const char*	code() const{ return _code; }
	
	/**
	 * @brief 设置K线切片的合约代码
	 * 
	 * 修改当前K线切片对应的标的合约代码
	 * 
	 * @param code 新的合约代码
	 */
	inline void		setCode(const char* code){ wt_strcpy(_code, code); }


	/**
	 * @brief 提取K线切片中指定字段的数据序列
	 * 
	 * 将指定区间内的某个K线字段（如开盘价、收盘价等）提取出来
	 * 形成一个WTSValueArray数值数组，方便进行指标计算等操作
	 * 这是策略计算中非常常用的一个方法，用于从K线中提取出某个数据序列
	 * 
	 * @param type 要提取的K线字段类型，如开盘价、最高价、最低价、收盘价、成交量、日期等
	 * @param head 区间起始位置，默认为0（第一个K线）
	 * @param tail 区间结束位置，默认为-1（最后一个K线）
	 * @return WTSValueArray* 提取出的数据数组对象指针，K线为空时返回NULL
	 */
	WTSValueArray*	extractData(WTSKlineFieldType type, int32_t head = 0, int32_t tail = -1) const
	{
		if (_count == 0)  // 如果K线切片为空
			return NULL;

		head = translateIdx(head);  // 转换起始位置索引
		tail = translateIdx(tail);  // 转换结束位置索引

		int32_t begin = max(0, min(head, tail));  // 确保起始位置不小于0
		int32_t end = min(max(head, tail), size() - 1);  // 确保结束位置不超出范围

		WTSValueArray *vArray = NULL;

		vArray = WTSValueArray::create();  // 创建一个新的数值数组对象

		for (int32_t i = begin; i <= end; i++)  // 遍历指定区间内的所有K线
		{
			const WTSBarStruct& day = *at(i);  // 获取当前K线
			switch (type)  // 根据字段类型提取不同的值
			{
				case KFT_OPEN:  // 开盘价
					vArray->append(day.open);
					break;
				case KFT_HIGH:  // 最高价
					vArray->append(day.high);
					break;
				case KFT_LOW:  // 最低价
					vArray->append(day.low);
					break;
				case KFT_CLOSE:  // 收盘价
					vArray->append(day.close);
					break;
				case KFT_VOLUME:  // 成交量
					vArray->append(day.vol);
					break;
				case KFT_SVOLUME:  // 带符号的成交量（根据收盘价和开盘价的关系标记正负）
					if (day.vol > INT_MAX)  // 处理超大成交量
						vArray->append(1 * ((day.close > day.open) ? 1 : -1));  // 数值太大时只保留符号
					else
						vArray->append((int32_t)day.vol * ((day.close > day.open) ? 1 : -1));  // 正常情况保留成交量和符号
					break;
				case KFT_DATE:  // 日期
					vArray->append(day.date);
					break;
				case KFT_TIME:  // 时间
					vArray->append((double)day.time);  // 转换为double类型存储
			}
		}

		return vArray;  // 返回提取的数据数组
	}
};

/**
 * @brief K线数据类
 * 
 * 用于存储和管理K线数据的类，直接使用std::vector容器存储WTSBarStruct结构体
 * 与WTSKlineSlice不同，WTSKlineData使用连续内存存储所有K线数据
 * 这个类主要用于K线数据的管理和基本查询操作
 * 
 * K线数据的内部使用WTSBarStruct结构体
 * 由于K线数据单独使用的可能性较低，所以WTSBarStruct不做WTSObject派生类的封装
 */
class WTSKlineData : public WTSObject
{
public:
	/** WTSBarList类型定义，用于存储K线数据的容器 */
	typedef std::vector<WTSBarStruct> WTSBarList;

protected:
	char			m_strCode[32];     ///< 合约代码
	WTSKlinePeriod	m_kpPeriod;      ///< K线周期类型（分钟、日线等）
	uint32_t		m_uTimes;        ///< 周期倍数（如5分钟、15分钟等的倍数）
	bool			m_bUnixTime;      ///< 是否是时间戳格式，目前只在秒线上有效
	WTSBarList		m_vecBarData;    ///< K线数据容器，使用std::vector存储
	bool			m_bClosed;        ///< 是否是闭合K线，表示最后一条K线是否已完成

protected:
	/**
	 * @brief 构造函数（保护类型）
	 * 
	 * 初始化K线数据对象，默认为1分钟周期、倍数为1、非时间戳格式、闭合K线
	 * 构造函数为保护类型，禁止外部直接创建对象，必须通过静态create方法创建
	 */
	WTSKlineData()
		:m_kpPeriod(KP_Minute1)  // 默认为1分钟周期
		,m_uTimes(1)             // 默认倍数为1
		,m_bUnixTime(false)      // 默认非时间戳格式
		,m_bClosed(true)         // 默认为闭合K线
	{

	}

	/**
	 * @brief 转换索引
	 * 
	 * 将索引转换为合法的数组索引，支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 原始索引，可以为负数
	 * @return int32_t 转换后的合法索引
	 */
	inline int32_t		translateIdx(int32_t idx) const
	{
		if(idx < 0)  // 处理负索引
		{
			return max(0, (int32_t)m_vecBarData.size() + idx);  // 将负索引转换为从末尾计算的正索引
		}

		return idx;  // 正索引直接返回
	}

public:
	/**
	 * @brief 创建一个K线数据对象
	 * 
	 * 静态工厂方法，创建并初始化一个K线数据对象
	 * 这是创建WTSKlineData对象的唯一方法
	 * 
	 * @param code 合约代码
	 * @param size 初始分配的数据容量
	 * @return WTSKlineData* 创建的K线数据对象指针
	 */
	static WTSKlineData* create(const char* code, uint32_t size)
	{
		WTSKlineData *pRet = new WTSKlineData;  // 创建新对象
		pRet->m_vecBarData.resize(size);         // 预分配存储空间
		wt_strcpy(pRet->m_strCode, code);        // 设置合约代码

		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 设置K线是否闭合
	 * 
	 * 闭合K线表示K线周期已经走完（如1分钟K线的1分钟时间已结束）
	 * 非闭合K线表示当前周期尚未结束，数据可能还会更新
	 * 
	 * @param bClosed 是否闭合
	 */
	inline void setClosed(bool bClosed){ m_bClosed = bClosed; }
	
	/**
	 * @brief 判断K线是否闭合
	 * 
	 * @return bool 如果K线已闭合返回true，否则返回false
	 */
	inline bool isClosed() const{ return m_bClosed; }

	/**
	 * @brief 设置K线周期和倍数
	 * 
	 * 设置K线的基础周期类型（如分钟、日线等）和周期倍数（如5分钟、15分钟等）
	 * 
	 * @param period 基础周期类型
	 * @param times 周期倍数，默认为1
	 */
	inline void	setPeriod(WTSKlinePeriod period, uint32_t times = 1){ m_kpPeriod = period; m_uTimes = times; }

	/**
	 * @brief 设置是否使用Unix时间戳格式
	 * 
	 * 目前主要在秒线级别的K线上使用
	 * 
	 * @param bEnabled 是否启用Unix时间戳格式，默认为true
	 */
	inline void	setUnixTime(bool bEnabled = true){ m_bUnixTime = bEnabled; }

	/**
	 * @brief 获取K线周期类型
	 * 
	 * @return WTSKlinePeriod K线周期类型
	 */
	inline WTSKlinePeriod	period() const{ return m_kpPeriod; }
	
	/**
	 * @brief 获取K线周期倍数
	 * 
	 * @return uint32_t 周期倍数
	 */
	inline uint32_t		times() const{ return m_uTimes; }
	
	/**
	 * @brief 判断是否使用Unix时间戳格式
	 * 
	 * @return bool 如果使用Unix时间戳格式返回true，否则返回false
	 */
	inline bool			isUnixTime() const{ return m_bUnixTime; }

	/**
	 * @brief 查找指定范围内的最高价格
	 * 
	 * 在指定的K线区间内查找最高价格（high字段）
	 * 支持负索引，会将索引转换为实际位置
	 * 
	 * @param head 区间起始位置
	 * @param tail 区间结束位置
	 * @return double 区间内的最高价格，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double		maxprice(int32_t head, int32_t tail) const
	{
		head = translateIdx(head);  // 转换起始位置索引
		tail = translateIdx(tail);  // 转换结束位置索引

		uint32_t begin = min(head, tail);  // 确保begin是较小的索引
		uint32_t end = max(head, tail);    // 确保end是较大的索引

		if(begin >= m_vecBarData.size() || end > m_vecBarData.size())  // 检查索引是否超出范围
			return INVALID_DOUBLE;  // 超出范围返回无效值

		double maxValue = m_vecBarData[begin].high;  // 初始化最大值为起始位置的最高价
		for(uint32_t i = begin; i <= end; i++)  // 遍历指定区间内的所有K线
		{
			maxValue = max(maxValue, m_vecBarData[i].high);  // 更新最大值
		}

		return maxValue;  // 返回最高价格
	}

	/**
	 * @brief 查找指定范围内的最低价格
	 * 
	 * 在指定的K线区间内查找最低价格（low字段）
	 * 支持负索引，会将索引转换为实际位置
	 * 
	 * @param head 区间起始位置
	 * @param tail 区间结束位置
	 * @return double 区间内的最低价格，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double		minprice(int32_t head, int32_t tail) const
	{
		head = translateIdx(head);  // 转换起始位置索引
		tail = translateIdx(tail);  // 转换结束位置索引

		uint32_t begin = min(head, tail);  // 确保begin是较小的索引
		uint32_t end = max(head, tail);    // 确保end是较大的索引

		if(begin >= m_vecBarData.size() || end > m_vecBarData.size())  // 检查索引是否超出范围
			return INVALID_DOUBLE;  // 超出范围返回无效值

		double minValue = m_vecBarData[begin].low;  // 初始化最小值为起始位置的最低价
		for(uint32_t i = begin; i <= end; i++)  // 遍历指定区间内的所有K线
		{
			minValue = min(minValue, m_vecBarData[i].low);  // 更新最小值
		}

		return minValue;  // 返回最低价格
	}
	
	/**
	 * @brief 返回K线数据的总数量
	 * 
	 * 获取K线数据容器中的K线条数
	 * 
	 * @return uint32_t K线总数
	 */
	inline uint32_t	size() const{return m_vecBarData.size();}
	
	/**
	 * @brief 判断K线数据是否为空
	 * 
	 * 检查K线数据容器是否不包含任何K线
	 * 
	 * @return bool 如果没有K线数据返回true，否则返回false
	 */
	inline bool IsEmpty() const{ return m_vecBarData.empty(); }

	/**
	 * @brief 获取K线数据的合约代码
	 * 
	 * 返回当前K线数据对应的标的合约代码
	 * 
	 * @return const char* 合约代码字符串
	 */
	inline const char*	code() const{ return m_strCode; }
	
	/**
	 * @brief 设置K线数据的合约代码
	 * 
	 * 修改当前K线数据对应的标的合约代码
	 * 
	 * @param code 新的合约代码
	 */
	inline void		setCode(const char* code){ wt_strcpy(m_strCode, code); }

	/**
	 * @brief 读取指定位置的开盘价
	 * 
	 * 获取指定索引位置K线的开盘价
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的开盘价，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double	open(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_DOUBLE;  // 无效索引返回无效值

		return m_vecBarData[idx].open;  // 返回指定位置的开盘价
	}

	/**
	 * @brief 读取指定位置的最高价
	 * 
	 * 获取指定索引位置K线的最高价
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的最高价，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double	high(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_DOUBLE;  // 无效索引返回无效值

		return m_vecBarData[idx].high;  // 返回指定位置的最高价
	}

	/**
	 * @brief 读取指定位置的最低价
	 * 
	 * 获取指定索引位置K线的最低价
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的最低价，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double	low(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_DOUBLE;  // 无效索引返回无效值

		return m_vecBarData[idx].low;  // 返回指定位置的最低价
	}

	/**
	 * @brief 读取指定位置的收盘价
	 * 
	 * 获取指定索引位置K线的收盘价
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的收盘价，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double	close(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_DOUBLE;  // 无效索引返回无效值

		return m_vecBarData[idx].close;  // 返回指定位置的收盘价
	}

	/**
	 * @brief 读取指定位置的成交量
	 * 
	 * 获取指定索引位置K线的成交量
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的成交量，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double	volume(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_DOUBLE;  // 无效索引返回无效值

		return m_vecBarData[idx].vol;  // 返回指定位置的成交量
	}

	/**
	 * @brief 读取指定位置的持仓量（总持仓量）
	 * 
	 * 获取指定索引位置K线的持仓量（在期货和期权合约中表示市场上的总持仓合约数量）
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的持仓量，如果索引超出范围则返回INVALID_UINT32
	 */
	inline double	openinterest(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_UINT32;  // 无效索引返回无效值

		return m_vecBarData[idx].hold;  // 返回指定位置的持仓量
	}

	/**
	 * @brief 读取指定位置的增仓量
	 * 
	 * 获取指定索引位置K线的增仓量（与前一个K线相比的持仓量变化）
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的增仓量，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double	additional(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_DOUBLE;  // 无效索引返回无效值

		return m_vecBarData[idx].add;  // 返回指定位置的增仓量
	}	

	/**
	 * @brief 读取指定位置的买一价（买盘价）
	 * 
	 * 获取指定索引位置K线的买一价（市场上最高买入出价）
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的买一价，如果索引超出范围则返回INVALID_UINT32
	 */
	inline double	bidprice(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if (idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_UINT32;  // 无效索引返回无效值

		return m_vecBarData[idx].bid;  // 返回指定位置的买一价
	}

	/**
	 * @brief 读取指定位置的卖一价（卖盘价）
	 * 
	 * 获取指定索引位置K线的卖一价（市场上最低卖出出价）
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的卖一价，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double	askprice(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if (idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_DOUBLE;  // 无效索引返回无效值

		return m_vecBarData[idx].ask;  // 返回指定位置的卖一价
	}

	/**
	 * @brief 读取指定位置的成交额
	 * 
	 * 获取指定索引位置K线的成交金额（价格 * 成交量的总和）
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return double 指定位置的成交额，如果索引超出范围则返回INVALID_DOUBLE
	 */
	inline double	money(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_DOUBLE;  // 无效索引返回无效值

		return m_vecBarData[idx].money;  // 返回指定位置的成交额
	}

	/**
	 * @brief 读取指定位置的日期
	 * 
	 * 获取指定索引位置K线的日期，通常以YYYYMMDD的格式表示
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return uint32_t 指定位置的日期，如果索引超出范围则返回INVALID_UINT32
	 */
	inline uint32_t	date(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_UINT32;  // 无效索引返回无效值

		return m_vecBarData[idx].date;  // 返回指定位置的日期
	}

	/**
	 * @brief 读取指定位置的时间
	 * 
	 * 获取指定索引位置K线的时间，通常以HHMMSS或时间戳的格式表示
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return uint64_t 指定位置的时间，如果索引超出范围则返回INVALID_UINT32
	 */
	inline uint64_t	time(int32_t idx) const
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return INVALID_UINT32;  // 无效索引返回无效值

		return m_vecBarData[idx].time;  // 返回指定位置的时间
	}

	/**
	 * @brief 提取K线数据中指定字段的数据序列
	 * 
	 * 将指定区间内的某个K线字段（如开盘价、收盘价等）提取出来
	 * 形成一个WTSValueArray数值数组，方便进行指标计算等操作
	 * 这是策略计算中非常常用的一个方法，用于从K线中提取出某个数据序列
	 * 
	 * @param type 要提取的K线字段类型，如开盘价、最高价、最低价、收盘价、成交量、日期等
	 * @param head 区间起始位置，默认为0（第一个K线）
	 * @param tail 区间结束位置，默认为-1（最后一个K线）
	 * @return WTSValueArray* 提取出的数据数组对象指针，如果索引超出范围则返回NULL
	 */
	WTSValueArray*	extractData(WTSKlineFieldType type, int32_t head = 0, int32_t tail = -1) const
	{
		head = translateIdx(head);  // 转换起始位置索引
		tail = translateIdx(tail);  // 转换结束位置索引

		uint32_t begin = min(head, tail);  // 确保begin是较小的索引
		uint32_t end = max(head, tail);    // 确保end是较大的索引

		if(begin >= m_vecBarData.size() || end >= (int32_t)m_vecBarData.size())  // 检查索引是否超出范围
			return NULL;  // 超出范围返回NULL

		WTSValueArray *vArray = NULL;

		vArray = WTSValueArray::create();  // 创建一个新的数值数组对象

		for(uint32_t i = 0; i < m_vecBarData.size(); i++)  // 遍历所有K线数据
		{
			const WTSBarStruct& day = m_vecBarData.at(i);  // 获取当前K线
			switch(type)  // 根据字段类型提取不同的值
			{
				case KFT_OPEN:  // 开盘价
					vArray->append(day.open);
					break;
				case KFT_HIGH:  // 最高价
					vArray->append(day.high);
					break;
				case KFT_LOW:  // 最低价
					vArray->append(day.low);
					break;
				case KFT_CLOSE:  // 收盘价
					vArray->append(day.close);
					break;
				case KFT_VOLUME:  // 成交量
					vArray->append(day.vol);
					break;
				case KFT_SVOLUME:  // 带符号的成交量（根据收盘价和开盘价的关系标记正负）
					if(day.vol > INT_MAX)  // 处理超大成交量
						vArray->append(1 * ((day.close > day.open) ? 1 : -1));  // 数值太大时只保留符号
					else
						vArray->append((int32_t)day.vol * ((day.close > day.open)?1:-1));  // 正常情况保留成交量和符号
					break;
				case KFT_DATE:  // 日期
					vArray->append(day.date);
					break;
				case KFT_TIME:  // 时间
					vArray->append((double)day.time);  // 转换为double类型存储
			}
		}

		return vArray;  // 返回提取的数据数组
	}

public:
	/**
	 * @brief 获取K线数据内部vector容器的引用
	 * 
	 * 返回K线数据内部存储容器的引用，允许直接访问和操作底层存储
	 * 警告：使用这个方法需要非常小心，因为它返回内部存储的引用，可以直接修改数据
	 * 
	 * @return WTSBarList& K线数据容器的引用
	 */
	inline WTSBarList& getDataRef(){ return m_vecBarData; }

	/**
	 * @brief 访问指定位置的K线数据
	 * 
	 * 获取指定索引位置K线的指针，允许读取和修改K线数据
	 * 支持负索引（如-1表示最后一条K线）
	 * 
	 * @param idx 索引位置，支持负索引
	 * @return WTSBarStruct* 指定位置K线数据的指针，如果索引超出范围则返回NULL
	 */
	inline WTSBarStruct*	at(int32_t idx)
	{
		idx = translateIdx(idx);  // 转换索引，处理负索引情况

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())  // 检查索引是否有效
			return NULL;  // 无效索引返回NULL
		return &m_vecBarData[idx];  // 返回指定位置K线的指针
	}

	/**
	 * @brief 释放K线数据对象资源
	 * 
	 * 当引用计数降为1时（即只剩当前一个引用），清空K线数据容器
	 * 调用父类的release方法处理引用计数等操作
	 * 这个方法是清理对象占用的资源的关键方法
	 */
	virtual void release()
	{
		if(isSingleRefs())  // 检查是否只剩一个引用
		{
			m_vecBarData.clear();  // 清空K线数据容器
		}		

		WTSObject::release();  // 调用父类的release方法
	}

	/**
	 * @brief 追加一条K线数据
	 * 
	 * 将新的K线数据添加到容器的末尾
	 * 如果新K线的日期和时间与最后一条K线相同，则更新最后一条K线（而不是添加新的）
	 * 这通常用于实时数据的处理，当收到新的市场数据更新时
	 * 
	 * @param bar 要添加的新K线数据
	 */
	inline void	appendBar(const WTSBarStruct& bar)
	{
		if(m_vecBarData.empty())  // 如果容器为空
		{
			m_vecBarData.emplace_back(bar);  // 直接添加新K线
		}
		else
		{
			WTSBarStruct* lastBar = at(-1);  // 获取最后一条K线
			if(lastBar->date==bar.date && lastBar->time==bar.time)  // 如果日期和时间相同
			{
				memcpy(lastBar, &bar, sizeof(WTSBarStruct));  // 更新最后一条K线数据
			}
			else
			{
				m_vecBarData.emplace_back(bar);  // 添加新K线数据
			}
		}
	}
};



/**
 * @brief Tick数据对象
 * 
 * 用于存储和管理实时市场数据（Tick数据）的类
 * 内部封装WTSTickStruct结构体，包含了实时行情数据
 * 如最新成交价、委买委卖相关数据、成交量、持仓量等
 * 继承WTSPoolObject类实现对象池管理，提高内存使用效率
 * 
 * 封装的主要目的是出于跨语言的考虑，将底层数据结构封装成类，便于对外提供接口
 * 同时提供了丰富的方法来访问其中的各个字段
 */
class WTSTickData : public WTSPoolObject<WTSTickData>
{
public:
	/**
	 * @brief 构造函数
	 * 
	 * 初始化Tick数据对象，将合约指针初始化为NULL
	 */
	WTSTickData() :m_pContract(NULL) {}

	/**
	 * @brief 创建一个Tick数据对象
	 * 
	 * 静态工厂方法，创建并初始化一个Tick数据对象
	 * 从对象池中分配内存，并设置合约代码
	 * 
	 * @param stdCode 标准化合约代码
	 * @return WTSTickData* 创建的Tick数据对象指针
	 */
	static inline WTSTickData* create(const char* stdCode)
	{
		WTSTickData* pRet = WTSTickData::allocate();  // 从对象池中分配内存
		auto len = strlen(stdCode);  // 获取合约代码长度
		memcpy(pRet->m_tickStruct.code, stdCode, len);  // 复制合约代码
		pRet->m_tickStruct.code[len] = 0;  // 确保字符串结束符

		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 根据tick结构体创建一个tick数据对象
	 * 
	 * 静态工厂方法，从现有的WTSTickStruct结构体创建一个Tick数据对象
	 * 直接复制结构体的内容到新对象中
	 * 
	 * @param tickData 要复制的Tick结构体
	 * @return WTSTickData* 创建的Tick数据对象指针
	 */
	static inline WTSTickData* create(WTSTickStruct& tickData)
	{
		WTSTickData* pRet = allocate();  // 从对象池中分配内存
		memcpy(&pRet->m_tickStruct, &tickData, sizeof(WTSTickStruct));  // 复制结构体数据

		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 设置Tick数据的合约代码
	 * 
	 * 修改Tick数据对象对应的合约代码
	 * 
	 * @param code 新的合约代码
	 * @param len 合约代码长度，默认为0（自动计算）
	 */
	inline void setCode(const char* code, std::size_t len = 0)
	{
		len = (len == 0) ? strlen(code) : len;  // 如果长度为0，自动计算字符串长度

		memcpy(m_tickStruct.code, code, len);  // 复制合约代码
		m_tickStruct.code[len] = '\0';  // 确保字符串结束符
	}

	/**
	 * @brief 获取Tick数据的合约代码
	 * 
	 * 返回Tick数据对象对应的标的合约代码
	 * 
	 * @return const char* 合约代码字符串
	 */
	inline const char* code() const{ return m_tickStruct.code; }

	/**
	 * @brief 获取Tick数据的交易所代码
	 * 
	 * 返回Tick数据对应的交易所/市场代码
	 * 
	 * @return const char* 交易所代码字符串
	 */
	inline const char*	exchg() const{ return m_tickStruct.exchg; }

	/**
	 * @brief 获取Tick数据的最新成交价
	 * 
	 * 返回最近一笔成交的价格，即当前市场新成交的最新价格
	 * 
	 * @return double 最新成交价
	 */
	inline double	price() const{ return m_tickStruct.price; }

	/**
	 * @brief 获取Tick数据的开盘价
	 * 
	 * 返回当日开盘的第一笔成交价格
	 * 
	 * @return double 开盘价
	 */
	inline double	open() const{ return m_tickStruct.open; }

	/**
	 * @brief 获取Tick数据的最高价
	 * 
	 * 返回当日的最高成交价格
	 * 
	 * @return double 最高价
	 */
	inline double	high() const{ return m_tickStruct.high; }

	/**
	 * @brief 获取Tick数据的最低价
	 * 
	 * 返回当日的最低成交价格
	 * 
	 * @return double 最低价
	 */
	inline double	low() const{ return m_tickStruct.low; }

	/**
	 * @brief 获取昨日收盘价
	 * 
	 * 返回昨日的收盘价格，对于期货合约这是昨日结算价
	 * 
	 * @return double 昨收价
	 */
	inline double	preclose() const{ return m_tickStruct.pre_close; }
	
	/**
	 * @brief 获取昨日结算价
	 * 
	 * 返回昨日的结算价格，主要用于期货或期权合约
	 * 
	 * @return double 昨日结算价
	 */
	inline double	presettle() const{ return m_tickStruct.pre_settle; }
	
	/**
	 * @brief 获取昨日持仓量
	 * 
	 * 返回昨日的市场持仓量，主要用于期货或期权合约
	 * 
	 * @return double 昨日持仓量
	 */
	inline double	preinterest() const{ return m_tickStruct.pre_interest; }

	/**
	 * @brief 获取涨停板价格
	 * 
	 * 返回当日的涨停板价格上限
	 * 
	 * @return double 涨停板价格
	 */
	inline double	upperlimit() const{ return m_tickStruct.upper_limit; }
	
	/**
	 * @brief 获取跌停板价格
	 * 
	 * 返回当日的跌停板价格下限
	 * 
	 * @return double 跌停板价格
	 */
	inline double	lowerlimit() const{ return m_tickStruct.lower_limit; }
	
	/**
	 * @brief 获取累计成交量
	 * 
	 * 返回当日的总成交量（自开盘以来累计）
	 * 
	 * @return double 累计成交量
	 */
	inline double	totalvolume() const{ return m_tickStruct.total_volume; }

	/**
	 * @brief 获取当前成交量
	 * 
	 * 返回最近一笔新成交的成交量，非累计值
	 * 
	 * @return double 当前成交量
	 */
	inline double	volume() const{ return m_tickStruct.volume; }

	/**
	 * @brief 获取当前结算价
	 * 
	 * 返回当前的结算价格，主要用于期货和期权合约
	 * 注意结算价通常在收盘后才会最终确定
	 * 
	 * @return double 结算价
	 */
	inline double	settlepx() const{ return m_tickStruct.settle_price; }

	/**
	 * @brief 获取当前持仓量
	 * 
	 * 返回当前的市场持仓量（总持仓量），主要用于期货和期权合约
	 * 表示市场上所有未平仓合约的数量
	 * 
	 * @return double 当前持仓量
	 */
	inline double	openinterest() const{ return m_tickStruct.open_interest; }

	/**
	 * @brief 获取持仓量变化（增仓量）
	 * 
	 * 返回相对于前一个tick的持仓量变化量
	 * 可比较成交量和增仓量判断市场交易的性质
	 * 
	 * @return double 持仓量变化（增仓量）
	 */
	inline double	additional() const{ return m_tickStruct.diff_interest; }

	/**
	 * @brief 获取累计成交额
	 * 
	 * 返回当日的总成交金额（自开盘以来累计）
	 * 
	 * @return double 累计成交额
	 */
	inline double	totalturnover() const{ return m_tickStruct.total_turnover; }

	/**
	 * @brief 获取当前成交额
	 * 
	 * 返回最近一笔新成交的成交金额，非累计值
	 * 
	 * @return double 当前成交额
	 */
	inline double	turnover() const{ return m_tickStruct.turn_over; }

	/**
	 * @brief 获取交易日期
	 * 
	 * 返回当前的交易日期，通常以YYYYMMDD的格式表示
	 * 交易日是交易所规定的市场交易日，可能与自然日不同
	 * 
	 * @return uint32_t 交易日期（YYYYMMDD格式）
	 */
	inline uint32_t	tradingdate() const{ return m_tickStruct.trading_date; }

	/**
	 * @brief 获取数据发生日期
	 * 
	 * 返回tick数据实际生成的日期，通常以YYYYMMDD的格式表示
	 * 这是实际数据更新的日期，可能与交易日不同（如夜盘交易）
	 * 
	 * @return uint32_t 数据发生日期（YYYYMMDD格式）
	 */
	inline uint32_t	actiondate() const{ return m_tickStruct.action_date; }

	/**
	 * @brief 获取数据发生时间
	 * 
	 * 返回tick数据实际生成的时间，通常以HHMMSS的格式表示
	 * 这是实际数据更新的时间点，包含秒数信息
	 * 
	 * @return uint32_t 数据发生时间（HHMMSS格式）
	 */
	inline uint32_t	actiontime() const{ return m_tickStruct.action_time; }


	/**
	 * @brief 获取指定档位的委买价（买盘价）
	 * 
	 * 返回指定档位的委买报价，即市场上买入订单的报价
	 * 通常idx=0表示买一价（当前最高买入报价），idx=1表示买二价，以此类推
	 * 
	 * @param idx 档位索引，范围为0-9（女参见中国规定的最多显示10档行情）
	 * @return double 指定档位的委买价，如果指定的档位无效则返回-1
	 */
	inline double		bidprice(int idx) const
	{
		if(idx < 0 || idx >= 10)  // 检查档位索引是否有效
			return -1;  // 索引无效返回-1

		return m_tickStruct.bid_prices[idx];  // 返回指定档位的委买价
	}

	/**
	 * @brief 获取指定档位的委卖价（卖盘价）
	 * 
	 * 返回指定档位的委卖报价，即市场上卖出订单的报价
	 * 通常idx=0表示卖一价（当前最低卖出报价），idx=1表示卖二价，以此类推
	 * 
	 * @param idx 档位索引，范围为0-9（参见中国规定的最多显示10档行情）
	 * @return double 指定档位的委卖价，如果指定的档位无效则返回-1
	 */
	inline double		askprice(int idx) const
	{
		if(idx < 0 || idx >= 10)  // 检查档位索引是否有效
			return -1;  // 索引无效返回-1

		return m_tickStruct.ask_prices[idx];  // 返回指定档位的委卖价
	}

	/**
	 * @brief 获取指定档位的委买量（买盘量）
	 * 
	 * 返回指定档位的委买数量，即市场上在指定价格上的买入订单数量
	 * 与askprice对应使用，如idx=0表示在买一价上的买入订单数量
	 * 
	 * @param idx 档位索引，范围为0-9
	 * @return double 指定档位的委买量，如果指定的档位无效则返回-1
	 */
	inline double	bidqty(int idx) const
	{
		if(idx < 0 || idx >= 10)  // 检查档位索引是否有效
			return -1;  // 索引无效返回-1

		return m_tickStruct.bid_qty[idx];  // 返回指定档位的委买量
	}

	/**
	 * @brief 获取指定档位的委卖量（卖盘量）
	 * 
	 * 返回指定档位的委卖数量，即市场上在指定价格上的卖出订单数量
	 * 与bidprice对应使用，如idx=0表示在卖一价上的卖出订单数量
	 * 
	 * @param idx 档位索引，范围为0-9
	 * @return double 指定档位的委卖量，如果指定的档位无效则返回-1
	 */
	inline double	askqty(int idx) const
	{
		if(idx < 0 || idx >= 10)  // 检查档位索引是否有效
			return -1;  // 索引无效返回-1

		return m_tickStruct.ask_qty[idx];  // 返回指定档位的委卖量
	}

	/*
	 *	返回tick结构体的引用
	 */
	inline WTSTickStruct&	getTickStruct(){ return m_tickStruct; }

	/**
	 * @brief 设置Tick数据对应的合约信息
	 * 
	 * 关联合约信息对象到当前Tick数据对象
	 * 合约信息包含合约名称、代码、手续费率等详细信息
	 * 
	 * @param cInfo 合约信息对象指针
	 */
	inline void setContractInfo(WTSContractInfo* cInfo) { m_pContract = cInfo; }
	
	/**
	 * @brief 获取Tick数据对应的合约信息
	 * 
	 * 返回Tick数据对象关联的合约信息对象
	 * 
	 * @return WTSContractInfo* 合约信息对象指针
	 */
	inline WTSContractInfo* getContractInfo() const { return m_pContract; }

private:
	WTSTickStruct		m_tickStruct;  ///< Tick数据实际存储的结构体
	WTSContractInfo*	m_pContract;   ///< 合约信息对象指针
};

/**
 * @brief 委托队列数据类
 * 
 * 用于存储和管理市场委托队列数据（Order Queue数据）
 * 内部封装WTSOrdQueStruct结构体，包含了委托队列的相关信息
 * 委托队列数据提供了市场深度信息，可用于分析交易行为
 */
class WTSOrdQueData : public WTSObject
{
public:
	/**
	 * @brief 创建一个委托队列数据对象
	 * 
	 * 静态工厂方法，创建并初始化一个委托队列数据对象
	 * 并设置合约代码
	 * 
	 * @param code 合约代码
	 * @return WTSOrdQueData* 创建的委托队列数据对象指针
	 */
	static inline WTSOrdQueData* create(const char* code)
	{
		WTSOrdQueData* pRet = new WTSOrdQueData;  // 创建新对象
		wt_strcpy(pRet->m_oqStruct.code, code);    // 设置合约代码
		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 根据委托队列结构体创建一个委托队列数据对象
	 * 
	 * 静态工厂方法，从现有的WTSOrdQueStruct结构体创建一个委托队列数据对象
	 * 直接复制结构体的内容到新对象中
	 * 
	 * @param ordQueData 要复制的委托队列结构体
	 * @return WTSOrdQueData* 创建的委托队列数据对象指针
	 */
	static inline WTSOrdQueData* create(WTSOrdQueStruct& ordQueData)
	{
		WTSOrdQueData* pRet = new WTSOrdQueData;  // 创建新对象
		memcpy(&pRet->m_oqStruct, &ordQueData, sizeof(WTSOrdQueStruct));  // 复制结构体数据

		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 获取委托队列结构体引用
	 * 
	 * 返回委托队列数据内部结构体的引用，允许直接访问和修改其内容
	 * 
	 * @return WTSOrdQueStruct& 委托队列结构体的引用
	 */
	inline WTSOrdQueStruct& getOrdQueStruct(){return m_oqStruct;}

	/**
	 * @brief 获取委托队列数据的交易所代码
	 * 
	 * @return const char* 交易所代码字符串
	 */
	inline const char* exchg() const{ return m_oqStruct.exchg; }
	
	/**
	 * @brief 获取委托队列数据的合约代码
	 * 
	 * @return const char* 合约代码字符串
	 */
	inline const char* code() const{ return m_oqStruct.code; }
	
	/**
	 * @brief 获取交易日期
	 * 
	 * @return uint32_t 交易日期（YYYYMMDD格式）
	 */
	inline uint32_t tradingdate() const{ return m_oqStruct.trading_date; }
	
	/**
	 * @brief 获取数据发生日期
	 * 
	 * @return uint32_t 数据发生日期（YYYYMMDD格式）
	 */
	inline uint32_t actiondate() const{ return m_oqStruct.action_date; }
	
	/**
	 * @brief 获取数据发生时间
	 * 
	 * @return uint32_t 数据发生时间（HHMMSS格式）
	 */
	inline uint32_t actiontime() const { return m_oqStruct.action_time; }

	/**
	 * @brief 设置委托队列数据的合约代码
	 * 
	 * @param code 合约代码字符串
	 */
	inline void		setCode(const char* code) { wt_strcpy(m_oqStruct.code, code); }

	/**
	 * @brief 设置委托队列数据对应的合约信息
	 * 
	 * @param cInfo 合约信息对象指针
	 */
	inline void setContractInfo(WTSContractInfo* cInfo) { m_pContract = cInfo; }
	
	/**
	 * @brief 获取委托队列数据对应的合约信息
	 * 
	 * @return WTSContractInfo* 合约信息对象指针
	 */
	inline WTSContractInfo* getContractInfo() const { return m_pContract; }

private:
	WTSOrdQueStruct		m_oqStruct;    ///< 委托队列数据结构体
	WTSContractInfo*	m_pContract;    ///< 合约信息对象指针
};

/**
 * @brief 委托明细数据类
 * 
 * 用于存储和管理市场委托明细数据（Order Detail数据）
 * 内部封装WTSOrdDtlStruct结构体，包含了单笔委托的详细信息
 * 委托明细数据包含委托价格、数量、方向等信息，可用于分析交易意图
 */
class WTSOrdDtlData : public WTSObject
{
public:
	/**
	 * @brief 创建一个委托明细数据对象
	 * 
	 * 静态工厂方法，创建并初始化一个委托明细数据对象
	 * 并设置合约代码
	 * 
	 * @param code 合约代码
	 * @return WTSOrdDtlData* 创建的委托明细数据对象指针
	 */
	static inline WTSOrdDtlData* create(const char* code)
	{
		WTSOrdDtlData* pRet = new WTSOrdDtlData;  // 创建新对象
		wt_strcpy(pRet->m_odStruct.code, code);   // 设置合约代码
		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 根据委托明细结构体创建一个委托明细数据对象
	 * 
	 * 静态工厂方法，从现有的WTSOrdDtlStruct结构体创建一个委托明细数据对象
	 * 直接复制结构体的内容到新对象中
	 * 
	 * @param odData 要复制的委托明细结构体
	 * @return WTSOrdDtlData* 创建的委托明细数据对象指针
	 */
	static inline WTSOrdDtlData* create(WTSOrdDtlStruct& odData)
	{
		WTSOrdDtlData* pRet = new WTSOrdDtlData;  // 创建新对象
		memcpy(&pRet->m_odStruct, &odData, sizeof(WTSOrdDtlStruct));  // 复制结构体数据

		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 获取委托明细结构体引用
	 * 
	 * 返回委托明细数据内部结构体的引用，允许直接访问和修改其内容
	 * 
	 * @return WTSOrdDtlStruct& 委托明细结构体的引用
	 */
	inline WTSOrdDtlStruct& getOrdDtlStruct(){ return m_odStruct; }

	/**
	 * @brief 获取委托明细数据的交易所代码
	 * 
	 * @return const char* 交易所代码字符串
	 */
	inline const char* exchg() const{ return m_odStruct.exchg; }
	
	/**
	 * @brief 获取委托明细数据的合约代码
	 * 
	 * @return const char* 合约代码字符串
	 */
	inline const char* code() const{ return m_odStruct.code; }
	
	/**
	 * @brief 获取交易日期
	 * 
	 * @return uint32_t 交易日期（YYYYMMDD格式）
	 */
	inline uint32_t tradingdate() const{ return m_odStruct.trading_date; }
	
	/**
	 * @brief 获取数据发生日期
	 * 
	 * @return uint32_t 数据发生日期（YYYYMMDD格式）
	 */
	inline uint32_t actiondate() const{ return m_odStruct.action_date; }
	
	/**
	 * @brief 获取数据发生时间
	 * 
	 * @return uint32_t 数据发生时间（HHMMSS格式）
	 */
	inline uint32_t actiontime() const { return m_odStruct.action_time; }

	/**
	 * @brief 设置委托明细数据的合约代码
	 * 
	 * @param code 合约代码字符串
	 */
	inline void		setCode(const char* code) { wt_strcpy(m_odStruct.code, code); }

	/**
	 * @brief 设置委托明细数据对应的合约信息
	 * 
	 * @param cInfo 合约信息对象指针
	 */
	inline void setContractInfo(WTSContractInfo* cInfo) { m_pContract = cInfo; }
	
	/**
	 * @brief 获取委托明细数据对应的合约信息
	 * 
	 * @return WTSContractInfo* 合约信息对象指针
	 */
	inline WTSContractInfo* getContractInfo() const { return m_pContract; }


private:
	WTSOrdDtlStruct		m_odStruct;   ///< 委托明细数据结构体
	WTSContractInfo*	m_pContract;   ///< 合约信息对象指针
};

/**
 * @brief 成交数据类
 * 
 * 用于存储和管理市场成交数据（Transaction数据）
 * 内部封装WTSTransStruct结构体，包含了单笔成交的详细信息
 * 成交数据包含成交价格、数量、方向等信息，是实际成交的记录
 */
class WTSTransData : public WTSObject
{
public:
	/**
	 * @brief 创建一个成交数据对象
	 * 
	 * 静态工厂方法，创建并初始化一个成交数据对象
	 * 并设置合约代码
	 * 
	 * @param code 合约代码
	 * @return WTSTransData* 创建的成交数据对象指针
	 */
	static inline WTSTransData* create(const char* code)
	{
		WTSTransData* pRet = new WTSTransData;  // 创建新对象
		wt_strcpy(pRet->m_tsStruct.code, code);  // 设置合约代码
		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 根据成交结构体创建一个成交数据对象
	 * 
	 * 静态工厂方法，从现有的WTSTransStruct结构体创建一个成交数据对象
	 * 直接复制结构体的内容到新对象中
	 * 
	 * @param transData 要复制的成交结构体
	 * @return WTSTransData* 创建的成交数据对象指针
	 */
	static inline WTSTransData* create(WTSTransStruct& transData)
	{
		WTSTransData* pRet = new WTSTransData;  // 创建新对象
		memcpy(&pRet->m_tsStruct, &transData, sizeof(WTSTransStruct));  // 复制结构体数据

		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 获取成交数据的交易所代码
	 * 
	 * @return const char* 交易所代码字符串
	 */
	inline const char* exchg() const{ return m_tsStruct.exchg; }
	
	/**
	 * @brief 获取成交数据的合约代码
	 * 
	 * @return const char* 合约代码字符串
	 */
	inline const char* code() const{ return m_tsStruct.code; }
	
	/**
	 * @brief 获取交易日期
	 * 
	 * @return uint32_t 交易日期（YYYYMMDD格式）
	 */
	inline uint32_t tradingdate() const{ return m_tsStruct.trading_date; }
	
	/**
	 * @brief 获取数据发生日期
	 * 
	 * @return uint32_t 数据发生日期（YYYYMMDD格式）
	 */
	inline uint32_t actiondate() const{ return m_tsStruct.action_date; }
	
	/**
	 * @brief 获取数据发生时间
	 * 
	 * @return uint32_t 数据发生时间（HHMMSS格式）
	 */
	inline uint32_t actiontime() const { return m_tsStruct.action_time; }

	/**
	 * @brief 获取成交结构体引用
	 * 
	 * 返回成交数据内部结构体的引用，允许直接访问和修改其内容
	 * 
	 * @return WTSTransStruct& 成交结构体的引用
	 */
	inline WTSTransStruct& getTransStruct(){ return m_tsStruct; }

	/**
	 * @brief 设置成交数据的合约代码
	 * 
	 * @param code 合约代码字符串
	 */
	inline void		setCode(const char* code) { wt_strcpy(m_tsStruct.code, code); }

	/**
	 * @brief 设置成交数据对应的合约信息
	 * 
	 * @param cInfo 合约信息对象指针
	 */
	inline void setContractInfo(WTSContractInfo* cInfo) { m_pContract = cInfo; }
	
	/**
	 * @brief 获取成交数据对应的合约信息
	 * 
	 * @return WTSContractInfo* 合约信息对象指针
	 */
	inline WTSContractInfo* getContractInfo() const { return m_pContract; }

private:
	WTSTransStruct		m_tsStruct;    ///< 成交数据结构体
	WTSContractInfo*	m_pContract;   ///< 合约信息对象指针
};

/**
 * @brief 历史Tick数据类
 * 
 * 用于存储和管理历史Tick数据的类
 * 内部使用std::vector<WTSTickStruct>作为容器存储Tick数据
 * 支持复权因子处理，可用于回测和策略研究

 */
class WTSHisTickData : public WTSObject
{
protected:
	/**
	 * @brief 合约代码
	 * 
	 * 存储历史Tick数据对应的合约代码
	 */
	char						m_strCode[32];
	
	/**
	 * @brief Tick数据容器
	 * 
	 * 使用std::vector存储历史Tick数据，支持动态扩容
	 */
	std::vector<WTSTickStruct>	m_ayTicks;
	
	/**
	 * @brief 是否只包含有效数据
	 * 
	 * 标记是否只包含有效的Tick数据，用于过滤无效数据
	 */
	bool						m_bValidOnly;
	
	/**
	 * @brief 复权因子
	 * 
	 * 用于价格数据的复权处理，支持不同复权因子的历史数据
	 */
	double						m_dFactor;

	/**
	 * @brief 构造函数（保护类型）
	 * 
	 * 初始化历史Tick数据对象，默认不仅包含有效数据，复权因子为1.0
	 * 构造函数为保护类型，禁止外部直接创建对象，必须通过静态create方法创建
	 */
	WTSHisTickData() :m_bValidOnly(false), m_dFactor(1.0){}

public:
	/**
	 * @brief 创建指定大小的历史Tick数据对象
	 * 
	 * 静态工厂方法，创建并初始化一个历史Tick数据对象
	 * 内部的数组应预先分配指定大小，优化内存使用
	 * 
	 * @param stdCode 标准化合约代码
	 * @param nSize 预先分配的大小，默认为0（不预分配）
	 * @param bValidOnly 是否仅包含有效数据，默认为false
	 * @param factor 复权因子，默认为1.0（不复权）
	 * @return WTSHisTickData* 创建的历史Tick数据对象指针
	 */
	static inline WTSHisTickData* create(const char* stdCode, unsigned int nSize = 0, bool bValidOnly = false, double factor = 1.0)
	{
		WTSHisTickData *pRet = new WTSHisTickData;  // 创建新对象
		wt_strcpy(pRet->m_strCode, stdCode);       // 设置合约代码
		pRet->m_ayTicks.resize(nSize);             // 预分配容器大小
		pRet->m_bValidOnly = bValidOnly;           // 设置是否仅包含有效数据
		pRet->m_dFactor = factor;                  // 设置复权因子

		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 创建历史Tick数据对象（不预分配容量版本）
	 * 
	 * 静态工厂方法，创建并初始化一个历史Tick数据对象
	 * 与带容量参数的create方法不同，这个方法不预分配内部容器的大小
	 * 
	 * @param stdCode 标准化合约代码
	 * @param bValidOnly 是否仅包含有效数据，默认为false
	 * @param factor 复权因子，默认为1.0（不复权）
	 * @return WTSHisTickData* 创建的历史Tick数据对象指针
	 */
	static inline WTSHisTickData* create(const char* stdCode, bool bValidOnly = false, double factor = 1.0)
	{
		WTSHisTickData *pRet = new WTSHisTickData;  // 创建新对象
		wt_strcpy(pRet->m_strCode, stdCode);       // 设置合约代码
		pRet->m_bValidOnly = bValidOnly;           // 设置是否仅包含有效数据
		pRet->m_dFactor = factor;                  // 设置复权因子

		return pRet;  // 返回创建的对象
	}

	/**
	 * @brief 获取历史Tick数据的条数
	 * 
	 * 返回当前存储的Tick数据总数
	 * 
	 * @return uint32_t Tick数据总条数
	 */
	inline uint32_t	size() const{ return m_ayTicks.size(); }
	
	/**
	 * @brief 判断历史Tick数据是否为空
	 * 
	 * 检查当前是否没有存储任何Tick数据
	 * 
	 * @return bool 如果没有数据返回true，否则返回false
	 */
	inline bool		empty() const{ return m_ayTicks.empty(); }

	/**
	 * @brief 获取历史Tick数据对应的合约代码
	 * 
	 * 返回当前Tick数据对应的标的合约代码
	 * 
	 * @return const char* 合约代码字符串
	 */
	inline const char*		code() const{ return m_strCode; }

	/**
	 * @brief 获取指定位置的Tick数据
	 * 
	 * 返回指定索引位置的Tick数据结构体指针
	 * 
	 * @param idx 索引位置
	 * @return WTSTickStruct* 指定位置的Tick数据结构体指针，如果索引无效则返回NULL
	 */
	inline WTSTickStruct*	at(uint32_t idx)
	{
		if (m_ayTicks.empty() || idx >= m_ayTicks.size())  // 检查索引是否有效
			return NULL;  // 无效索引返回NULL

		return &m_ayTicks[idx];  // 返回指定位置的Tick数据指针
	}

	/**
	 * @brief 获取Tick数据容器的引用
	 * 
	 * 返回Tick数据内部存储容器的引用，允许直接访问和修改
	 * 警告：使用这个方法需要非常小心，因为它返回内部存储的引用
	 * 
	 * @return std::vector<WTSTickStruct>& Tick数据容器的引用
	 */
	inline std::vector<WTSTickStruct>& getDataRef() { return m_ayTicks; }

	/**
	 * @brief 判断是否仅包含有效数据
	 * 
	 * 返回当前对象的有效数据标记状态
	 * 
	 * @return bool 如果仅包含有效数据返回true，否则返回false
	 */
	inline bool isValidOnly() const{ return m_bValidOnly; }

	/**
	 * @brief 追加一条Tick数据
	 * 
	 * 将新的Tick数据添加到历史数据容器的末尾
	 * 并根据复权因子对价格数据进行调整
	 * 
	 * @param ts 要添加的Tick数据结构体
	 */
	inline void	appendTick(const WTSTickStruct& ts)
	{
		m_ayTicks.emplace_back(ts);  // 添加新的Tick数据
		// 复权修正，根据复权因子对价格数据进行调整
		m_ayTicks.back().price *= m_dFactor;  // 调整最新成交价
		m_ayTicks.back().open *= m_dFactor;   // 调整开盘价
		m_ayTicks.back().high *= m_dFactor;   // 调整最高价
		m_ayTicks.back().low *= m_dFactor;    // 调整最低价
	}
};

//////////////////////////////////////////////////////////////////////////
/**
 * @brief Tick数据切片类
 * 
 * 从连续的tick缓存中创建的数据切片
 * 重要特性：切片并没有真实地复制内存，而只是引用原始数据并记录开始和结尾的位置
 * 这种实现方式提高了效率，但使用场景需要小心，因为它依赖于基础数据对象的生命周期
 * 支持多个数据块的管理，适用于非连续内存的数据集合
 */
class WTSTickSlice : public WTSObject
{
private:
	char			_code[MAX_INSTRUMENT_LENGTH];       ///< 合约代码
	typedef std::pair<WTSTickStruct*, uint32_t> TickBlock;  ///< Tick数据块类型，第一个元素是数据指针，第二个是数据数量
	std::vector<TickBlock> _blocks;              ///< Tick数据块容器，存储多个数据块的引用
	uint32_t		_count;                      ///< 总数据数量

protected:
	/**
	 * @brief 构造函数（保护类型）
	 * 
	 * 初始化Tick切片对象，清空数据块容器
	 * 构造函数为保护类型，禁止外部直接创建对象，必须通过静态create方法创建
	 */
	WTSTickSlice() { _blocks.clear(); }
	
	/**
	 * @brief 转换索引位置
	 * 
	 * 将传入的索引转换为有效的正向索引位置
	 * 支持负数索引，负数索引表示从尾部开始向前数
	 * 例如：-1表示最后一个元素，-2表示倒数第二个元素
	 * 
	 * @param idx 输入的索引值（可正可负）
	 * @return int32_t 转换后的有效正向索引
	 */
	inline int32_t		translateIdx(int32_t idx) const
	{
		if (idx < 0)  // 如果是负数索引
		{
			return max(0, (int32_t)_count + idx);  // 计算从末尾开始的位置，并确保不小于0
		}

		return idx;  // 正数索引直接返回
	}

public:
	/**
	 * @brief 创建一个Tick数据切片对象
	 * 
	 * 静态工厂方法，创建并初始化一个Tick数据切片对象
	 * 可以初始化为空切片，也可以指定初始数据块
	 * 
	 * @param code 合约代码
	 * @param ticks Tick数据结构体数组指针，默认为NULL
	 * @param count 数据数量，默认为0
	 * @return WTSTickSlice* 创建的Tick数据切片对象指针
	 */
	static inline WTSTickSlice* create(const char* code, WTSTickStruct* ticks = NULL, uint32_t count = 0)
	{
		//if (ticks == NULL || count == 0)  // 注释掉的代码，允许创建空切片
		//	return NULL;

		WTSTickSlice* slice = new WTSTickSlice();  // 创建新对象
		wt_strcpy(slice->_code, code);             // 设置合约代码
		if(ticks != NULL)  // 如果提供了数据，添加到切片中
		{
			slice->_blocks.emplace_back(TickBlock(ticks, count));  // 添加数据块
			slice->_count = count;  // 设置总数据数量
		}

		return slice;  // 返回创建的对象
	}

	/**
	 * @brief 追加一个Tick数据块
	 * 
	 * 将新的Tick数据块添加到切片对象的末尾
	 * 这允许切片管理多个不连续的内存数据块
	 * 
	 * @param ticks 要添加的Tick数据结构体数组指针
	 * @param count 数据数量
	 * @return bool 如果添加成功返回true，否则返回false
	 */
	inline bool appendBlock(WTSTickStruct* ticks, uint32_t count)
	{
		if (ticks == NULL || count == 0)  // 检查参数是否有效
			return false;  // 无效参数，返回false

		_count += count;  // 增加总数据数量
		_blocks.emplace_back(TickBlock(ticks, count));  // 添加数据块到容器尾部
		return true;  // 添加成功
	}

	/**
	 * @brief 在指定位置插入一个Tick数据块
	 * 
	 * 在切片对象的指定位置插入新的Tick数据块
	 * 与追加不同，这允许在特定位置插入数据块
	 * 
	 * @param idx 要插入的位置索引
	 * @param ticks 要插入的Tick数据结构体数组指针
	 * @param count 数据数量
	 * @return bool 如果插入成功返回true，否则返回false
	 */
	inline bool insertBlock(std::size_t idx, WTSTickStruct* ticks, uint32_t count)
	{
		if (ticks == NULL || count == 0)  // 检查参数是否有效
			return false;  // 无效参数，返回false

		_count += count;  // 增加总数据数量
		_blocks.insert(_blocks.begin()+idx, TickBlock(ticks, count));  // 在指定位置插入数据块
		return true;  // 插入成功
	}

	/**
	 * @brief 获取数据块数量
	 * 
	 * 返回当前切片对象中包含的数据块数量
	 * 
	 * @return std::size_t 数据块数量
	 */
	inline std::size_t	get_block_counts() const
	{
		return _blocks.size();  // 返回数据块容器的大小
	}

	/**
	 * @brief 获取指定数据块的地址
	 * 
	 * 返回指定索引的数据块的内存地址指针
	 * 
	 * @param blkIdx 数据块索引
	 * @return WTSTickStruct* 数据块的内存地址指针，如果索引无效则返回NULL
	 */
	inline WTSTickStruct*	get_block_addr(std::size_t blkIdx)
	{
		if (blkIdx >= _blocks.size())  // 检查索引是否有效
			return NULL;  // 无效索引返回NULL

		return _blocks[blkIdx].first;  // 返回指定数据块的内存地址
	}

	/**
	 * @brief 获取指定数据块的大小
	 * 
	 * 返回指定索引的数据块包含的数据数量
	 * 
	 * @param blkIdx 数据块索引
	 * @return uint32_t 数据块的大小（数据数量），如果索引无效则返回INVALID_UINT32
	 */
	inline uint32_t get_block_size(std::size_t blkIdx)
	{
		if (blkIdx >= _blocks.size())  // 检查索引是否有效
			return INVALID_UINT32;  // 无效索引返回INVALID_UINT32

		return _blocks[blkIdx].second;  // 返回指定数据块的大小
	}

	/**
	 * @brief 获取切片的总数据数量
	 * 
	 * 返回当前切片对象包含的所有数据块中的总数据数量
	 * 
	 * @return uint32_t 总数据数量
	 */
	inline uint32_t size() const{ return _count; }

	/**
	 * @brief 判断切片是否为空
	 * 
	 * 检查当前切片对象是否没有包含任何数据
	 * 
	 * @return bool 如果没有数据返回true，否则返回false
	 */
	inline bool empty() const{ return (_count == 0); }

	/**
	 * @brief 获取指定位置的Tick数据
	 * 
	 * 返回切片中指定索引位置的Tick数据结构体
	 * 支持负数索引，可介面向后索引
	 * 该方法会跨数据块进行定位，将多个块看作一个连续的数据集
	 * 
	 * @param idx 索引位置（可正可负）
	 * @return const WTSTickStruct* 指定位置的Tick数据结构体指针，如果索引无效则返回NULL
	 */
	inline const WTSTickStruct* at(int32_t idx)
	{
		if (_count == 0)  // 如果切片为空
			return NULL;  // 直接返回NULL

		idx = translateIdx(idx);  // 将索引转换为适当的正向索引
		do 
		{
			for(auto& item : _blocks)  // 遍历所有数据块
			{
				if ((uint32_t)idx >= item.second)  // 如果索引超过当前块的大小
					idx -= item.second;  // 减去当前块的大小，准备检查下一个块
				else
					return item.first + idx;  // 找到目标索引，返回对应的数据指针
			}
		} while (false);  // 仅循环一次的do-while结构
		return NULL;  // 如果没有找到合适的索引，返回NULL
	}
};

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 委托明细数据切片类
 * 
 * 从连续的委托明细数据缓存中创建的数据切片
 * 重要特性：切片并没有真实地复制内存，而只是引用原始数据并记录范围
 * 这种实现方式提高了效率，减少了内存开销，但使用时需要小心，因为它依赖于原始数据对象的生命周期
 * 与其他切片不同，此类只支持连续内存区域的数据管理
 */
class WTSOrdDtlSlice : public WTSObject
{
private:
	char				m_strCode[MAX_INSTRUMENT_LENGTH];  ///< 合约代码
	WTSOrdDtlStruct*	m_ptrBegin;                 ///< 委托明细数据数组的起始位置指针
	uint32_t			m_uCount;                      ///< 切片包含的数据数量

protected:
	/**
	 * @brief 构造函数（保护类型）
	 * 
	 * 初始化委托明细切片对象，数据指针初始化为NULL，数据数量初始化为0
	 * 构造函数为保护类型，禁止外部直接创建对象，必须通过静态create方法创建
	 */
	WTSOrdDtlSlice() :m_ptrBegin(NULL), m_uCount(0) {}
	
	/**
	 * @brief 转换索引位置
	 * 
	 * 将传入的索引转换为有效的正向索引位置
	 * 支持负数索引，负数索引表示从尾部开始向前数
	 * 例如：-1表示最后一个元素，-2表示倒数第二个元素
	 * 
	 * @param idx 输入的索引值（可正可负）
	 * @return int32_t 转换后的有效正向索引
	 */
	inline int32_t		translateIdx(int32_t idx) const
	{
		if (idx < 0)  // 如果是负数索引
		{
			return max(0, (int32_t)m_uCount + idx);  // 计算从末尾开始的位置，并确保不小于0
		}

		return idx;  // 正数索引直接返回
	}

public:
	/**
	 * @brief 创建一个委托明细数据切片对象
	 * 
	 * 静态工厂方法，创建并初始化一个委托明细数据切片对象
	 * 指定切片的起始位置和数据数量
	 * 
	 * @param code 合约代码
	 * @param firstItem 委托明细数据数组的起始指针
	 * @param count 切片包含的数据数量
	 * @return WTSOrdDtlSlice* 创建的委托明细数据切片对象指针，如果参数无效则返回NULL
	 */
	static inline WTSOrdDtlSlice* create(const char* code, WTSOrdDtlStruct* firstItem, uint32_t count)
	{
		if (count == 0 || firstItem == NULL)  // 检查参数是否有效
			return NULL;  // 无效参数返回NULL

		WTSOrdDtlSlice* slice = new WTSOrdDtlSlice();  // 创建新对象
		wt_strcpy(slice->m_strCode, code);            // 设置合约代码
		slice->m_ptrBegin = firstItem;                // 设置数据起始指针
		slice->m_uCount = count;                      // 设置数据数量

		return slice;  // 返回创建的对象
	}

	/**
	 * @brief 获取切片的数据数量
	 * 
	 * 返回当前切片对象包含的委托明细数据数量
	 * 
	 * @return uint32_t 数据数量
	 */
	inline uint32_t size() const { return m_uCount; }

	/**
	 * @brief 判断切片是否为空
	 * 
	 * 检查当前切片对象是否没有包含任何数据或数据指针为空
	 * 
	 * @return bool 如果没有数据或数据指针为空返回true，否则返回false
	 */
	inline bool empty() const { return (m_uCount == 0) || (m_ptrBegin == NULL); }

	/**
	 * @brief 获取指定位置的委托明细数据
	 * 
	 * 返回切片中指定索引位置的委托明细数据结构体
	 * 支持负数索引，可介面向后索引
	 * 使用指针偏移方式快速定位到指定位置的数据
	 * 
	 * @param idx 索引位置（可正可负）
	 * @return const WTSOrdDtlStruct* 指定位置的委托明细数据结构体指针，如果索引无效则返回NULL
	 */
	inline const WTSOrdDtlStruct* at(int32_t idx)
	{
		if (m_ptrBegin == NULL)  // 检查数据指针是否为空
			return NULL;  // 空指针返回NULL
		idx = translateIdx(idx);  // 将索引转换为适当的正向索引
		return m_ptrBegin + idx;  // 返回目标位置的数据指针
	}
};

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 委托队列数据切片类
 * 
 * 从连续的委托队列数据缓存中创建的数据切片
 * 重要特性：切片并没有真实地复制内存，而只是引用原始数据并记录范围
 * 这种实现方式提高了效率，减少了内存开销，但使用时需要小心，因为它依赖于原始数据对象的生命周期
 * 该类用于快速访问委托队列数据，支持连续内存区域的数据管理
 */
class WTSOrdQueSlice : public WTSObject
{
private:
	char				m_strCode[MAX_INSTRUMENT_LENGTH];  ///< 合约代码
	WTSOrdQueStruct*	m_ptrBegin;                 ///< 委托队列数据数组的起始位置指针
	uint32_t			m_uCount;                      ///< 切片包含的数据数量

protected:
	/**
	 * @brief 构造函数（保护类型）
	 * 
	 * 初始化委托队列切片对象，数据指针初始化为NULL，数据数量初始化为0
	 * 构造函数为保护类型，禁止外部直接创建对象，必须通过静态create方法创建
	 */
	WTSOrdQueSlice() :m_ptrBegin(NULL), m_uCount(0) {}
	
	/**
	 * @brief 转换索引位置
	 * 
	 * 将传入的索引转换为有效的正向索引位置
	 * 支持负数索引，负数索引表示从尾部开始向前数
	 * 例如：-1表示最后一个元素，-2表示倒数第二个元素
	 * 
	 * @param idx 输入的索引值（可正可负）
	 * @return int32_t 转换后的有效正向索引
	 */
	inline int32_t		translateIdx(int32_t idx) const
	{
		if (idx < 0)  // 如果是负数索引
		{
			return max(0, (int32_t)m_uCount + idx);  // 计算从末尾开始的位置，并确保不小于0
		}

		return idx;  // 正数索引直接返回
	}

public:
	/**
	 * @brief 创建一个委托队列数据切片对象
	 * 
	 * 静态工厂方法，创建并初始化一个委托队列数据切片对象
	 * 指定切片的起始位置和数据数量
	 * 
	 * @param code 合约代码
	 * @param firstItem 委托队列数据数组的起始指针
	 * @param count 切片包含的数据数量
	 * @return WTSOrdQueSlice* 创建的委托队列数据切片对象指针，如果参数无效则返回NULL
	 */
	static inline WTSOrdQueSlice* create(const char* code, WTSOrdQueStruct* firstItem, uint32_t count)
	{
		if (count == 0 || firstItem == NULL)  // 检查参数是否有效
			return NULL;  // 无效参数返回NULL

		WTSOrdQueSlice* slice = new WTSOrdQueSlice();  // 创建新对象
		wt_strcpy(slice->m_strCode, code);            // 设置合约代码
		slice->m_ptrBegin = firstItem;                // 设置数据起始指针
		slice->m_uCount = count;                      // 设置数据数量

		return slice;  // 返回创建的对象
	}

	/**
	 * @brief 获取切片的数据数量
	 * 
	 * 返回当前切片对象包含的委托队列数据数量
	 * 
	 * @return uint32_t 数据数量
	 */
	inline uint32_t size() const { return m_uCount; }

	/**
	 * @brief 判断切片是否为空
	 * 
	 * 检查当前切片对象是否没有包含任何数据或数据指针为空
	 * 
	 * @return bool 如果没有数据或数据指针为空返回true，否则返回false
	 */
	inline bool empty() const { return (m_uCount == 0) || (m_ptrBegin == NULL); }

	/**
	 * @brief 获取指定位置的委托队列数据
	 * 
	 * 返回切片中指定索引位置的委托队列数据结构体
	 * 支持负数索引，可介面向后索引
	 * 使用指针偏移方式快速定位到指定位置的数据
	 * 
	 * @param idx 索引位置（可正可负）
	 * @return const WTSOrdQueStruct* 指定位置的委托队列数据结构体指针，如果索引无效则返回NULL
	 */
	inline const WTSOrdQueStruct* at(int32_t idx)
	{
		if (m_ptrBegin == NULL)  // 检查数据指针是否为空
			return NULL;  // 空指针返回NULL
		idx = translateIdx(idx);  // 将索引转换为适当的正向索引
		return m_ptrBegin + idx;  // 返回目标位置的数据指针
	}
};

//////////////////////////////////////////////////////////////////////////
/**
 * @brief 逐笔成交数据切片类
 * 
 * 从连续的逐笔成交数据缓存中创建的数据切片
 * 重要特性：切片并没有真实地复制内存，而只是引用原始数据并记录范围
 * 这种实现方式提高了效率，减少了内存开销，但使用时需要小心，因为它依赖于原始数据对象的生命周期
 * 该类用于快速访问成交数据，支持连续内存区域的数据管理
 */
class WTSTransSlice : public WTSObject
{
private:
	char			m_strCode[MAX_INSTRUMENT_LENGTH];  ///< 合约代码
	WTSTransStruct*	m_ptrBegin;                ///< 逐笔成交数据数组的起始位置指针
	uint32_t		m_uCount;                     ///< 切片包含的数据数量

protected:
	/**
	 * @brief 构造函数（保护类型）
	 * 
	 * 初始化逐笔成交切片对象，数据指针初始化为NULL，数据数量初始化为0
	 * 构造函数为保护类型，禁止外部直接创建对象，必须通过静态create方法创建
	 */
	WTSTransSlice() :m_ptrBegin(NULL), m_uCount(0) {}
	
	/**
	 * @brief 转换索引位置
	 * 
	 * 将传入的索引转换为有效的正向索引位置
	 * 支持负数索引，负数索引表示从尾部开始向前数
	 * 例如：-1表示最后一个元素，-2表示倒数第二个元素
	 * 
	 * @param idx 输入的索引值（可正可负）
	 * @return int32_t 转换后的有效正向索引
	 */
	inline int32_t		translateIdx(int32_t idx) const
	{
		if (idx < 0)  // 如果是负数索引
		{
			return max(0, (int32_t)m_uCount + idx);  // 计算从末尾开始的位置，并确保不小于0
		}

		return idx;  // 正数索引直接返回
	}

public:
	/**
	 * @brief 创建一个逐笔成交数据切片对象
	 * 
	 * 静态工厂方法，创建并初始化一个逐笔成交数据切片对象
	 * 指定切片的起始位置和数据数量
	 * 
	 * @param code 合约代码
	 * @param firstItem 逐笔成交数据数组的起始指针
	 * @param count 切片包含的数据数量
	 * @return WTSTransSlice* 创建的逐笔成交数据切片对象指针，如果参数无效则返回NULL
	 */
	static inline WTSTransSlice* create(const char* code, WTSTransStruct* firstItem, uint32_t count)
	{
		if (count == 0 || firstItem == NULL)  // 检查参数是否有效
			return NULL;  // 无效参数返回NULL

		WTSTransSlice* slice = new WTSTransSlice();  // 创建新对象
		wt_strcpy(slice->m_strCode, code);          // 设置合约代码
		slice->m_ptrBegin = firstItem;              // 设置数据起始指针
		slice->m_uCount = count;                    // 设置数据数量

		return slice;  // 返回创建的对象
	}

	/**
	 * @brief 获取切片的数据数量
	 * 
	 * 返回当前切片对象包含的逐笔成交数据数量
	 * 
	 * @return uint32_t 数据数量
	 */
	inline uint32_t size() const { return m_uCount; }

	/**
	 * @brief 判断切片是否为空
	 * 
	 * 检查当前切片对象是否没有包含任何数据或数据指针为空
	 * 
	 * @return bool 如果没有数据或数据指针为空返回true，否则返回false
	 */
	inline bool empty() const { return (m_uCount == 0) || (m_ptrBegin == NULL); }

	/**
	 * @brief 获取指定位置的逐笔成交数据
	 * 
	 * 返回切片中指定索引位置的逐笔成交数据结构体
	 * 支持负数索引，可介面向后索引
	 * 使用指针偏移方式快速定位到指定位置的数据
	 * 
	 * @param idx 索引位置（可正可负）
	 * @return const WTSTransStruct* 指定位置的逐笔成交数据结构体指针，如果索引无效则返回NULL
	 */
	inline const WTSTransStruct* at(int32_t idx)
	{
		if (m_ptrBegin == NULL)  // 检查数据指针是否为空
			return NULL;  // 空指针返回NULL
		idx = translateIdx(idx);  // 将索引转换为适当的正向索引
		return m_ptrBegin + idx;  // 返回目标位置的数据指针
	}
};

NS_WTP_END