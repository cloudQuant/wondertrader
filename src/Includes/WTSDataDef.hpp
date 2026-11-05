/*!
 * \file WTSDataDef.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief Wt行情数据定义文件,包括tick、bar、orderqueue、orderdetail、transaction等数据
 */
#pragma once
#include <stdlib.h>
#include <vector>
#include <deque>
#include <string.h>
#include <chrono>
#include <memory>
#include <utility>

#include "WTSObject.hpp"

#include "WTSTypes.h"
#include "WTSMarcos.h"
#include "WTSStruct.h"
#include "WTSCollection.hpp"

using namespace std;

#ifdef _MSC_VER
#pragma warning(disable:4267)
#endif


NS_WTP_BEGIN
class WTSContractInfo;
/*
 *	数值数组的内部封装
 *	采用std::vector实现
 *	包含数据格式化字符串
 *	数值的数据类型为double
 */
class WTSValueArray : public WTSObject
{
protected:
	vector<double>	m_vecData;

public:
	/*
	 *	创建一个数值数组对象
	 *	@decimal 保留的小数点位数
	 */
	static WTSValueArray* create()
	{
		WTSValueArray* pRet = new WTSValueArray;
		pRet->m_vecData.clear();
		return pRet;
	}

	/*
	 *	读取数组的长度
	 */
	inline uint32_t	size() const{ return m_vecData.size(); }
	inline bool		empty() const{ return m_vecData.empty(); }

	/*
	 *	读取指定位置的数据
	 *	如果超出范围,则返回INVALID_VALUE
	 */
	inline double		at(uint32_t idx) const
	{
		const int64_t rawIdx = idx <= static_cast<uint32_t>(INT32_MAX)
			? static_cast<int64_t>(idx)
			: static_cast<int64_t>(idx) - (static_cast<int64_t>(UINT32_MAX) + 1);
		const int64_t translated = translateIdx(static_cast<int32_t>(rawIdx));
		if (translated < 0 || static_cast<uint64_t>(translated) >= m_vecData.size())
			return INVALID_DOUBLE;

		return m_vecData[static_cast<std::size_t>(translated)];
	}

	inline int64_t		translateIdx(int32_t idx) const
	{
		if(idx < 0)
		{
			return static_cast<int64_t>(m_vecData.size()) + static_cast<int64_t>(idx);
		}

		return idx;
	}

	/*
	 *	找到指定范围内的最大值
	 *	如果超出范围,则返回INVALID_VALUE
	 */
	double		maxvalue(int32_t head, int32_t tail, bool isAbs = false) const
	{
		if (m_vecData.empty())
			return INVALID_DOUBLE;

		const int64_t headIndex = translateIdx(head);
		const int64_t tailIndex = translateIdx(tail);
		const int64_t begin = min(headIndex, tailIndex);
		const int64_t end = max(headIndex, tailIndex);
		if (begin < 0 || end < 0 || static_cast<uint64_t>(end) >= m_vecData.size())
			return INVALID_DOUBLE;

		double maxValue = INVALID_DOUBLE;
		for (std::size_t i = static_cast<std::size_t>(begin); ; ++i)
		{
			if (m_vecData[i] != INVALID_DOUBLE)
			{
				if(maxValue == INVALID_DOUBLE)
					maxValue = isAbs?abs(m_vecData[i]):m_vecData[i];
				else
					maxValue = max(maxValue, isAbs?abs(m_vecData[i]):m_vecData[i]);
			}

			if (i == static_cast<std::size_t>(end))
				break;
		}

		//if (maxValue == INVALID_DOUBLE)
		//	maxValue = 0.0;

		return maxValue;
	}

	/*
	 *	找到指定范围内的最小值
	 *	如果超出范围,则返回INVALID_VALUE
	 */
	double		minvalue(int32_t head, int32_t tail, bool isAbs = false) const
	{
		if (m_vecData.empty())
			return INVALID_DOUBLE;

		const int64_t headIndex = translateIdx(head);
		const int64_t tailIndex = translateIdx(tail);
		const int64_t begin = min(headIndex, tailIndex);
		const int64_t end = max(headIndex, tailIndex);
		if (begin < 0 || end < 0 || static_cast<uint64_t>(end) >= m_vecData.size())
			return INVALID_DOUBLE;

		double minValue = INVALID_DOUBLE;
		for (std::size_t i = static_cast<std::size_t>(begin); ; ++i)
		{
			if (m_vecData[i] != INVALID_DOUBLE)
			{
				if(minValue == INVALID_DOUBLE)
					minValue = isAbs?abs(m_vecData[i]):m_vecData[i];
				else
					minValue = min(minValue, isAbs?abs(m_vecData[i]):m_vecData[i]);
			}

			if (i == static_cast<std::size_t>(end))
				break;
		}

		//if (minValue == INVALID_DOUBLE)
		//	minValue = 0.0;

		return minValue;
	}

	/*
	 *	在数组末尾添加数据
	 */
	inline void		append(double val)
	{
		m_vecData.emplace_back(val);
	}

	/*
	 *	设置指定位置的数据
	 */
	inline void		set(uint32_t idx, double val)
	{
		if(idx < 0 || idx >= m_vecData.size())
			return;

		m_vecData[idx] = val;
	}

	/*
	 *	重新分配数组大小,并设置默认值
	 */
	inline void		resize(uint32_t uSize, double val = INVALID_DOUBLE)
	{
		m_vecData.resize(uSize, val);
	}

	/*
	 *	重载操作符[]
	 *	用法同getValue接口
	 */
	inline double&		operator[](uint32_t idx)
	{
		return m_vecData[idx];
	}

	inline double		operator[](uint32_t idx) const
	{
		return m_vecData[idx];
	}

	inline std::vector<double>& getDataRef()
	{
		return m_vecData;
	}
};

/*
 *	切片共享所有权（迭代03 卡11=B，2026-11-03）
 *	切片仍然只保存指向底层缓冲区的指针（零拷贝），但生产者可以把底层缓冲区的
 *	所有者以引用计数的形式挂到切片上。缓存 clearCache/重载/淘汰只会释放缓存自己
 *	持有的那一份引用，仍被调用者持有的切片在 release 之前始终有效。
 *	约束：
 *	1、生产者挂上 owner 之后，不得再对该 owner 对应的缓冲区做结构性修改
 *	   （扩容、重分配、clear、swap），需要修改时应改写到新的缓冲区（写时复制）。
 *	2、owner 的析构代码位于创建它的模块中，持有切片期间不得卸载该模块。
 *	3、ABI：owner 成员追加在各切片类已有成员之后，原有成员偏移与内联访问函数不变；
 *	   使用旧头文件编译的插件仍可读取新宿主创建的切片，但不会获得 owner 接口。
 */
typedef std::shared_ptr<const void> WTSSliceOwnerPtr;

/*
 *	把一个 WTSObject 包装成切片 owner：retain 一次，owner 最后一个引用释放时 release
 */
inline WTSSliceOwnerPtr makeSliceOwner(WTSObject* obj)
{
	if (obj == NULL)
		return WTSSliceOwnerPtr();

	obj->retain();
	try
	{
		return WTSSliceOwnerPtr(obj, [](WTSObject* p) { p->release(); });
	}
	catch (...)
	{
		obj->release();
		throw;
	}
}

class WTSSliceOwners
{
public:
	WTSSliceOwners() {}

	/*
	 *	登记一个 owner，同一个 owner 只登记一次
	 *	返回 false 表示 owner 为空
	 */
	inline bool add(const WTSSliceOwnerPtr& owner)
	{
		if (!owner)
			return false;

		if (_inline[0].get() == owner.get() || _inline[1].get() == owner.get())
			return true;
		for (const WTSSliceOwnerPtr& item : _extra)
		{
			if (item.get() == owner.get())
				return true;
		}

		if (!_inline[0])
			_inline[0] = owner;
		else if (!_inline[1])
			_inline[1] = owner;
		else
			_extra.emplace_back(owner);
		return true;
	}

	inline std::size_t count() const
	{
		return (_inline[0] ? 1 : 0) + (_inline[1] ? 1 : 0) + _extra.size();
	}

private:
	WTSSliceOwnerPtr				_inline[2];
	std::vector<WTSSliceOwnerPtr>	_extra;
};

/*
 *	切片底层缓冲区的写时复制容器
 *	缓存用它代替直接持有 std::vector / boost::circular_buffer：
 *	- owner() 返回的引用计数可以挂到切片上；
 *	- 结构性修改（clear/resize/assign/push_back/swap 等）在缓冲区仍被切片引用时
 *	  先分离到新缓冲区，已借出的切片继续指向旧缓冲区；
 *	- 非 const 的元素访问（begin/data/operator[]/back 等）不会分离，
 *	  仅用于尚未借出或允许原地读写的场景；需要原地改写元素时调用 mutate()。
 */
template <typename Container>
class WTSSharedBuffer
{
public:
	typedef Container								container_type;
	typedef typename Container::value_type			value_type;
	typedef typename Container::size_type			size_type;
	typedef typename Container::iterator			iterator;
	typedef typename Container::const_iterator		const_iterator;

	WTSSharedBuffer() noexcept {}
	WTSSharedBuffer(const WTSSharedBuffer& rhs)
		: _data(rhs._data ? std::make_shared<Container>(*rhs._data) : std::shared_ptr<Container>()) {}
	WTSSharedBuffer(WTSSharedBuffer&& rhs) noexcept : _data(std::move(rhs._data)) {}

	WTSSharedBuffer& operator=(const WTSSharedBuffer& rhs)
	{
		if (this != &rhs)
			_data = rhs._data ? std::make_shared<Container>(*rhs._data) : std::shared_ptr<Container>();
		return *this;
	}

	WTSSharedBuffer& operator=(WTSSharedBuffer&& rhs) noexcept
	{
		if (this != &rhs)
			_data = std::move(rhs._data);
		return *this;
	}

	WTSSharedBuffer& operator=(Container&& rhs)
	{
		_data = std::make_shared<Container>(std::move(rhs));
		return *this;
	}

	WTSSharedBuffer& operator=(const Container& rhs)
	{
		_data = std::make_shared<Container>(rhs);
		return *this;
	}

	/* 只读视图，便于按原容器类型读取/复制 */
	inline operator const Container&() const { return get(); }

	/* 当前缓冲区的 owner；从未分配时为空 */
	inline WTSSliceOwnerPtr owner() const { return _data; }

	/* 当前缓冲区是否仍被切片等外部 owner 共享 */
	inline bool shared() const { return _data && _data.use_count() > 1; }

	inline const Container& get() const { return _data ? *_data : empty_container(); }

	/* 元素级访问，不分离 */
	inline Container& peek()
	{
		if (!_data)
			_data = std::make_shared<Container>();
		return *_data;
	}

	/* 结构性修改前调用：被共享时复制到新缓冲区 */
	inline Container& mutate()
	{
		if (!_data)
			_data = std::make_shared<Container>();
		else if (_data.use_count() > 1)
			_data = std::make_shared<Container>(*_data);
		return *_data;
	}

	/* 丢弃旧内容：被共享时直接换新缓冲区，不复制 */
	inline Container& renew()
	{
		if (!_data || _data.use_count() > 1)
			_data = std::make_shared<Container>();
		else
			_data->clear();
		return *_data;
	}

	inline size_type size() const { return get().size(); }
	inline bool empty() const { return get().empty(); }
	inline size_type capacity() const { return get().capacity(); }

	inline iterator begin() { return peek().begin(); }
	inline iterator end() { return peek().end(); }
	inline const_iterator begin() const { return get().begin(); }
	inline const_iterator end() const { return get().end(); }

	inline value_type* data() { return peek().data(); }
	inline const value_type* data() const { return get().data(); }

	inline value_type& operator[](size_type idx) { return peek()[idx]; }
	inline const value_type& operator[](size_type idx) const { return get()[idx]; }

	inline value_type& front() { return peek().front(); }
	inline const value_type& front() const { return get().front(); }
	inline value_type& back() { return peek().back(); }
	inline const value_type& back() const { return get().back(); }

	inline auto array_one() { return peek().array_one(); }
	inline auto array_two() { return peek().array_two(); }

	inline void clear() { renew(); }
	inline void resize(size_type n)
	{
		if (_data && _data.use_count() > 1)
		{
			// 被共享时只复制保留下来的前缀，避免 resize(0) 等场景复制整块旧数据
			const size_type keep = n < _data->size() ? n : _data->size();
			std::shared_ptr<Container> fresh = std::make_shared<Container>(_data->begin(), _data->begin() + keep);
			fresh->resize(n);
			_data = fresh;
		}
		else
		{
			peek().resize(n);
		}
	}
	inline void reserve(size_type n) { mutate().reserve(n); }
	inline void rset_capacity(size_type n) { mutate().rset_capacity(n); }
	inline void push_back(const value_type& v) { mutate().push_back(v); }
	template <typename... Args>
	inline void emplace_back(Args&&... args) { mutate().emplace_back(std::forward<Args>(args)...); }

	template <typename InputIt>
	inline void assign(InputIt first, InputIt last)
	{
		// 先构造新缓冲区，源区间即便来自自身也不会被提前破坏
		_data = std::make_shared<Container>(first, last);
	}

	/* 区间插入：先按下标记下位置，分离后再插入，避免使用旧缓冲区的迭代器 */
	template <typename InputIt>
	inline iterator insert(const_iterator pos, InputIt first, InputIt last)
	{
		const typename Container::difference_type idx = pos - get().begin();
		Container& c = mutate();
		return c.insert(c.begin() + idx, first, last);
	}

	/* 与普通容器交换内容：被共享时，旧内容以副本交出，新内容进入新缓冲区 */
	inline void swap(Container& rhs)
	{
		if (_data && _data.use_count() > 1)
		{
			Container old(*_data);
			_data = std::make_shared<Container>(std::move(rhs));
			rhs.swap(old);
		}
		else
		{
			peek().swap(rhs);
		}
	}

	/* 两个共享缓冲区之间只交换引用，已借出的切片不受影响 */
	inline void swap(WTSSharedBuffer& rhs) noexcept { _data.swap(rhs._data); }

private:
	static const Container& empty_container()
	{
		static const Container empty;
		return empty;
	}

private:
	std::shared_ptr<Container>	_data;
};

/*
 *	K线数据切片
 *	这个比较特殊,因为要拼接当日和历史的
 *	所以有两个开始地址
 */
class WTSKlineSlice : public WTSObject
{
private:
	char			_code[MAX_INSTRUMENT_LENGTH];
	WTSKlinePeriod	_period;
	uint32_t		_times;
	typedef std::pair<WTSBarStruct*, uint32_t> BarBlock;
	std::vector<BarBlock> _blocks;
	uint32_t		_count;
	WTSSliceOwners	_owners;	//追加成员，保持既有成员偏移不变

protected:
	WTSKlineSlice()
		: _period(KP_Minute1)
		, _times(1)
		, _count(0)
	{

	}

	inline int32_t		translateIdx(int32_t idx) const
	{
		int32_t totalCnt = _count;
		if (idx < 0)
		{
			return max(0, totalCnt + idx);
		}

		return idx;
	}

	inline int64_t translateIdxWide(int32_t idx) const
	{
		const int64_t translated = idx < 0
			? static_cast<int64_t>(_count) + static_cast<int64_t>(idx)
			: static_cast<int64_t>(idx);
		return translated < 0 ? 0 : translated;
	}

	inline WTSBarStruct* atWide(uint64_t idx)
	{
		if (idx >= static_cast<uint64_t>(_count))
			return NULL;

		for (auto& block : _blocks)
		{
			if (idx < block.second)
				return block.first + static_cast<std::size_t>(idx);
			idx -= block.second;
		}

		return NULL;
	}

	inline const WTSBarStruct* atWide(uint64_t idx) const
	{
		if (idx >= static_cast<uint64_t>(_count))
			return NULL;

		for (const auto& block : _blocks)
		{
			if (idx < block.second)
				return block.first + static_cast<std::size_t>(idx);
			idx -= block.second;
		}

		return NULL;
	}


public:
	static WTSKlineSlice* create(const char* code, WTSKlinePeriod period, uint32_t times, WTSBarStruct* bars = NULL, int32_t count = 0) noexcept
	{
		if (code == NULL || count < 0 || (count > 0 && bars == NULL))
			return NULL;
		// I03-NEW-083: same contract as WTSTickData::create -- a code that does not
		// fit _code with its terminator is rejected instead of overflowing it.
		std::size_t len = 0;
		while (len < MAX_INSTRUMENT_LENGTH && code[len] != '\0')
			++len;
		if (len == MAX_INSTRUMENT_LENGTH)
			return NULL;

		WTSKlineSlice *pRet = new WTSKlineSlice;
		memcpy(pRet->_code, code, len);
		pRet->_code[len] = '\0';
		pRet->_period = period;
		pRet->_times = times;
		if(bars)
			pRet->_blocks.emplace_back(BarBlock(bars, static_cast<uint32_t>(count)));
		pRet->_count = static_cast<uint32_t>(count);

		return pRet;
	}

	inline bool appendBlock(WTSBarStruct* bars, uint32_t count)
	{
		if (bars == NULL || count == 0 || count > UINT32_MAX - _count)
			return false;

		_blocks.emplace_back(BarBlock(bars, count));
		_count += count;
		return true;
	}

	inline std::size_t	get_block_counts() const
	{
		return _blocks.size();
	}

	inline WTSBarStruct*	get_block_addr(std::size_t blkIdx)
	{
		if (blkIdx >= _blocks.size())
			return NULL;

		return _blocks[blkIdx].first;
	}

	inline uint32_t get_block_size(std::size_t blkIdx)
	{
		if (blkIdx >= _blocks.size())
			return 0;

		return _blocks[blkIdx].second;
	}

	inline WTSBarStruct*	at(int32_t idx)
	{
		if (_count == 0)
			return NULL;

		const uint64_t translated = static_cast<uint64_t>(translateIdxWide(idx));
		return atWide(translated);
	}

	inline const WTSBarStruct*	at(int32_t idx) const
	{
		if (_count == 0)
			return NULL;

		const uint64_t translated = static_cast<uint64_t>(translateIdxWide(idx));
		return atWide(translated);
	}


	/*
	*	查找指定范围内的最大价格
	*	@head 起始位置
	*	@tail 结束位置
	*	如果位置超出范围,返回INVALID_VALUE
	*/
	inline double	maxprice(int32_t head, int32_t tail) const
	{
		if (_count == 0)
			return INVALID_DOUBLE;

		const int64_t count = static_cast<int64_t>(_count);
		const int64_t headIndex = head < 0 ? count + static_cast<int64_t>(head) : head;
		const int64_t tailIndex = tail < 0 ? count + static_cast<int64_t>(tail) : tail;
		if (headIndex < 0 && tailIndex < 0)
			return INVALID_DOUBLE;

		const int64_t first = min(headIndex, tailIndex);
		const int64_t last = max(headIndex, tailIndex);
		const int64_t begin = first < 0 ? 0 : first;
		const int64_t end = last >= count ? count - 1 : last;
		if (begin > end)
			return INVALID_DOUBLE;

		double maxValue = INVALID_DOUBLE;
		for (uint64_t i = static_cast<uint64_t>(begin); ; ++i)
		{
			const WTSBarStruct* bar = atWide(i);
			if (bar == NULL)
				return INVALID_DOUBLE;
			if (bar->high != INVALID_DOUBLE)
				maxValue = maxValue == INVALID_DOUBLE ? bar->high : max(maxValue, bar->high);
			if (i == static_cast<uint64_t>(end))
				break;
		}
		return maxValue;
	}

	/*
	*	查找指定范围内的最小价格
	*	@head 起始位置
	*	@tail 结束位置
	*	如果位置超出范围,返回INVALID_VALUE
	*/
	inline double	minprice(int32_t head, int32_t tail) const
	{
		if (_count == 0)
			return INVALID_DOUBLE;

		const int64_t count = static_cast<int64_t>(_count);
		const int64_t headIndex = head < 0 ? count + static_cast<int64_t>(head) : head;
		const int64_t tailIndex = tail < 0 ? count + static_cast<int64_t>(tail) : tail;
		if (headIndex < 0 && tailIndex < 0)
			return INVALID_DOUBLE;

		const int64_t first = min(headIndex, tailIndex);
		const int64_t last = max(headIndex, tailIndex);
		const int64_t begin = first < 0 ? 0 : first;
		const int64_t end = last >= count ? count - 1 : last;
		if (begin > end)
			return INVALID_DOUBLE;

		double minValue = INVALID_DOUBLE;
		for (uint64_t i = static_cast<uint64_t>(begin); ; ++i)
		{
			const WTSBarStruct* bar = atWide(i);
			if (bar == NULL)
				return INVALID_DOUBLE;
			if (bar->low != INVALID_DOUBLE)
				minValue = minValue == INVALID_DOUBLE ? bar->low : min(minValue, bar->low);
			if (i == static_cast<uint64_t>(end))
				break;
		}

		return minValue;
	}

	/*
	*	返回K线的大小
	*/
	inline int32_t	size() const{ return _count; }
	inline bool	empty() const{ return _count == 0; }

	/*
	*	返回K线对象的合约代码
	*/
	inline const char*	code() const{ return _code; }
	// I03-NEW-083: a NULL or overlong code keeps the current value.
	inline void		setCode(const char* code)
	{
		if (code == NULL)
			return;
		std::size_t len = 0;
		while (len < MAX_INSTRUMENT_LENGTH && code[len] != '\0')
			++len;
		if (len == MAX_INSTRUMENT_LENGTH)
			return;
		memcpy(_code, code, len);
		_code[len] = '\0';
	}

	/*
	 *	登记底层缓冲区 owner（卡11=B）：切片释放前 owner 不会被销毁
	 */
	inline bool addOwner(const WTSSliceOwnerPtr& owner) { return _owners.add(owner); }
	inline std::size_t ownerCount() const { return _owners.count(); }


	/*
	*	将指定范围内的某个特定字段的数据全部抓取出来
	*	并保存的一个数值数组中
	*	如果超出范围,则返回NULL
	*	@type 支持的类型有KT_OPEN、KT_HIGH、KT_LOW、KT_CLOSE,KFT_VOLUME、KT_DATE
	*/
	WTSValueArray*	extractData(WTSKlineFieldType type, int32_t head = 0, int32_t tail = -1) const noexcept
	{
		if (_count == 0)
			return NULL;

		const int64_t count = static_cast<int64_t>(_count);
		const int64_t headIndex = head < 0 ? count + static_cast<int64_t>(head) : head;
		const int64_t tailIndex = tail < 0 ? count + static_cast<int64_t>(tail) : tail;
		const int64_t begin = max<int64_t>(0, min(headIndex, tailIndex));
		const int64_t end = min<int64_t>(max(headIndex, tailIndex), count - 1);
		if (begin > end)
			return NULL;

		WTSValueArray *vArray = NULL;

		vArray = WTSValueArray::create();

		for (int64_t i = begin; i <= end; i++)
		{
			const WTSBarStruct* bar = atWide(static_cast<uint64_t>(i));
			if (bar == NULL)
			{
				vArray->release();
				return NULL;
			}

			const WTSBarStruct& day = *bar;
			switch (type)
			{
			case KFT_OPEN:
				vArray->append(day.open);
				break;
			case KFT_HIGH:
				vArray->append(day.high);
				break;
			case KFT_LOW:
				vArray->append(day.low);
				break;
			case KFT_CLOSE:
				vArray->append(day.close);
				break;
			case KFT_VOLUME:
				vArray->append(day.vol);
				break;
			case KFT_SVOLUME:
				if (day.vol > INT_MAX)
					vArray->append(1 * ((day.close > day.open) ? 1 : -1));
				else
					vArray->append((int32_t)day.vol * ((day.close > day.open) ? 1 : -1));
				break;
			case KFT_DATE:
				vArray->append(day.date);
				break;
			case KFT_TIME:
				vArray->append((double)day.time);
			}
		}

		return vArray;
	}
};

/*
 *	K线数据
 *	K线数据的内部数据使用WTSBarStruct
 *	WTSBarStruct是一个结构体
 *	因为K线数据单独使用的可能性较低
 *	所以不做WTSObject派生类的封装
 */
class WTSKlineData : public WTSObject
{
public:
	typedef std::vector<WTSBarStruct> WTSBarList;

protected:
	char			m_strCode[32];
	WTSKlinePeriod	m_kpPeriod;
	uint32_t		m_uTimes;
	bool			m_bUnixTime;	//是否是时间戳格式,目前只在秒线上有效
	WTSBarList		m_vecBarData;
	bool			m_bClosed;		//是否是闭合K线

protected:
	WTSKlineData()
		:m_kpPeriod(KP_Minute1)
		,m_uTimes(1)
		,m_bUnixTime(false)
		,m_bClosed(true)
	{

	}

	inline int32_t		translateIdx(int32_t idx) const
	{
		if(idx < 0)
		{
			return max(0, (int32_t)m_vecBarData.size() + idx);
		}

		return idx;
	}

public:
	/*
	 *	创建一个K线数据对象
	 *	@code 要创建的合约代码
	 *	@size 初始分配的数据长度
	 */
	static WTSKlineData* create(const char* code, uint32_t size) noexcept
	{
		WTSKlineData *pRet = new WTSKlineData;
		pRet->m_vecBarData.resize(size);
		wt_strcpy(pRet->m_strCode, code);

		return pRet;
	}

	inline void setClosed(bool bClosed){ m_bClosed = bClosed; }
	inline bool isClosed() const{ return m_bClosed; }

	/*
	 *	设置周期和步长
	 *	@period	基础周期
	 *	@times 倍数
	 */
	inline void	setPeriod(WTSKlinePeriod period, uint32_t times = 1){ m_kpPeriod = period; m_uTimes = times; }

	inline void	setUnixTime(bool bEnabled = true){ m_bUnixTime = bEnabled; }

	inline WTSKlinePeriod	period() const{ return m_kpPeriod; }
	inline uint32_t		times() const{ return m_uTimes; }
	inline bool			isUnixTime() const{ return m_bUnixTime; }

	/*
	 *	查找指定范围内的最大价格
	 *	@head 起始位置
	 *	@tail 结束位置
	 *	如果位置超出范围,返回INVALID_VALUE
	 */
	inline double		maxprice(int32_t head, int32_t tail) const
	{
		if (m_vecBarData.empty())
			return INVALID_DOUBLE;

		const int64_t count = static_cast<int64_t>(m_vecBarData.size());
		const int64_t headIndex = head < 0 ? count + static_cast<int64_t>(head) : head;
		const int64_t tailIndex = tail < 0 ? count + static_cast<int64_t>(tail) : tail;
		if (headIndex < 0 && tailIndex < 0)
			return INVALID_DOUBLE;

		const int64_t first = min(headIndex, tailIndex);
		const int64_t last = max(headIndex, tailIndex);
		const int64_t begin = first < 0 ? 0 : first;
		const int64_t end = last;

		if (begin > end || static_cast<uint64_t>(end) >= static_cast<uint64_t>(m_vecBarData.size()))
			return INVALID_DOUBLE;

		double maxValue = INVALID_DOUBLE;
		for (std::size_t i = static_cast<std::size_t>(begin); ; ++i)
		{
			const double high = m_vecBarData[i].high;
			if (high != INVALID_DOUBLE)
				maxValue = maxValue == INVALID_DOUBLE ? high : max(maxValue, high);
			if (i == static_cast<std::size_t>(end))
				break;
		}

		return maxValue;
	}

	/*
	 *	查找指定范围内的最小价格
	 *	@head 起始位置
	 *	@tail 结束位置
	 *	如果位置超出范围,返回INVALID_VALUE
	 */
	inline double		minprice(int32_t head, int32_t tail) const
	{
		if (m_vecBarData.empty())
			return INVALID_DOUBLE;

		const int64_t count = static_cast<int64_t>(m_vecBarData.size());
		const int64_t headIndex = head < 0 ? count + static_cast<int64_t>(head) : head;
		const int64_t tailIndex = tail < 0 ? count + static_cast<int64_t>(tail) : tail;
		if (headIndex < 0 && tailIndex < 0)
			return INVALID_DOUBLE;

		const int64_t first = min(headIndex, tailIndex);
		const int64_t last = max(headIndex, tailIndex);
		const int64_t begin = first < 0 ? 0 : first;
		const int64_t end = last;

		if (begin > end || static_cast<uint64_t>(end) >= static_cast<uint64_t>(m_vecBarData.size()))
			return INVALID_DOUBLE;

		double minValue = INVALID_DOUBLE;
		for (std::size_t i = static_cast<std::size_t>(begin); ; ++i)
		{
			const double low = m_vecBarData[i].low;
			if (low != INVALID_DOUBLE)
				minValue = minValue == INVALID_DOUBLE ? low : min(minValue, low);
			if (i == static_cast<std::size_t>(end))
				break;
		}

		return minValue;
	}
	
	/*
	 *	返回K线的大小
	 */
	inline uint32_t	size() const{return m_vecBarData.size();}
	inline bool IsEmpty() const{ return m_vecBarData.empty(); }

	/*
	 *	返回K线对象的合约代码
	 */
	inline const char*	code() const{ return m_strCode; }
	inline void		setCode(const char* code){ wt_strcpy(m_strCode, code); }

	/*
	 *	读取指定位置的开盘价
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	open(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_DOUBLE;

		return m_vecBarData[idx].open;
	}

	/*
	 *	读取指定位置的最高价
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	high(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_DOUBLE;

		return m_vecBarData[idx].high;
	}

	/*
	 *	读取指定位置的最低价
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	low(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_DOUBLE;

		return m_vecBarData[idx].low;
	}

	/*
	 *	读取指定位置的收盘价
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	close(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_DOUBLE;

		return m_vecBarData[idx].close;
	}

	/*
	 *	读取指定位置的成交量
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	volume(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_DOUBLE;

		return m_vecBarData[idx].vol;
	}

	/*
	 *	读取指定位置的总持
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	openinterest(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_UINT32;

		return m_vecBarData[idx].hold;
	}

	/*
	 *	读取指定位置的增仓
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	additional(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_DOUBLE;

		return m_vecBarData[idx].add;
	}	

	/*
	 *	读取指定位置的总持
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	bidprice(int32_t idx) const
	{
		idx = translateIdx(idx);

		if (idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_UINT32;

		return m_vecBarData[idx].bid;
	}

	/*
	 *	读取指定位置的增仓
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	askprice(int32_t idx) const
	{
		idx = translateIdx(idx);

		if (idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_DOUBLE;

		return m_vecBarData[idx].ask;
	}

	/*
	 *	读取指定位置的成交额
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline double	money(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_DOUBLE;

		return m_vecBarData[idx].money;
	}

	/*
	 *	读取指定位置的日期
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline uint32_t	date(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_UINT32;

		return m_vecBarData[idx].date;
	}

	/*
	 *	读取指定位置的时间
	 *	如果超出范围则返回INVALID_VALUE
	 */
	inline uint64_t	time(int32_t idx) const
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return INVALID_UINT32;

		return m_vecBarData[idx].time;
	}

	/*
	 *	将指定范围内的某个特定字段的数据全部抓取出来
	 *	并保存的一个数值数组中
	 *	如果超出范围,则返回NULL
	 *	@type 支持的类型有KT_OPEN、KT_HIGH、KT_LOW、KT_CLOSE,KFT_VOLUME、KT_DATE
	 */
	WTSValueArray*	extractData(WTSKlineFieldType type, int32_t head = 0, int32_t tail = -1) const
	{
		head = translateIdx(head);
		tail = translateIdx(tail);

		uint32_t begin = min(head, tail);
		uint32_t end = max(head, tail);

		if(begin >= m_vecBarData.size() || end >= m_vecBarData.size())
			return NULL;

		WTSValueArray *vArray = NULL;

		vArray = WTSValueArray::create();

		for(uint32_t i = 0; i < m_vecBarData.size(); i++)
		{
			const WTSBarStruct& day = m_vecBarData.at(i);
			switch(type)
			{
			case KFT_OPEN:
				vArray->append(day.open);
				break;
			case KFT_HIGH:
				vArray->append(day.high);
				break;
			case KFT_LOW:
				vArray->append(day.low);
				break;
			case KFT_CLOSE:
				vArray->append(day.close);
				break;
			case KFT_VOLUME:
				vArray->append(day.vol);
				break;
			case KFT_SVOLUME:
				if(day.vol > INT_MAX)
					vArray->append(1 * ((day.close > day.open) ? 1 : -1));
				else
					vArray->append((int32_t)day.vol * ((day.close > day.open)?1:-1));
				break;
			case KFT_DATE:
				vArray->append(day.date);
				break;
			case KFT_TIME:
				vArray->append((double)day.time);
			}
		}

		return vArray;
	}

public:
	/*
	 *	获取K线内部vector的引用
	 */
	inline WTSBarList& getDataRef(){ return m_vecBarData; }

	inline WTSBarStruct*	at(int32_t idx)
	{
		idx = translateIdx(idx);

		if(idx < 0 || idx >= (int32_t)m_vecBarData.size())
			return NULL;
		return &m_vecBarData[idx];
	}

	/*
	 *	释放K线数据
	 *	并delete所有的日线数据,清空vector
	 */
	virtual void release()
	{
		if(isSingleRefs())
		{
			m_vecBarData.clear();
		}		

		WTSObject::release();
	}

	/*
	 *	追加一条K线
	 */
	inline void	appendBar(const WTSBarStruct& bar)
	{
		if(m_vecBarData.empty())
		{
			m_vecBarData.emplace_back(bar);
		}
		else
		{
			WTSBarStruct* lastBar = at(-1);
			if(lastBar->date==bar.date && lastBar->time==bar.time)
			{
				memcpy(lastBar, &bar, sizeof(WTSBarStruct));
			}
			else
			{
				m_vecBarData.emplace_back(bar);
			}
		}
	}
};



/*
 *	Tick数据对象
 *	内部封装WTSTickStruct
 *	封装的主要目的是出于跨语言的考虑
 */
class WTSTickData : public WTSPoolObject<WTSTickData>
{
	friend WTSPoolObject<WTSTickData>::PoolType;

protected:
	WTSTickData() :m_pContract(NULL) {}

public:
	/*
	 *	创建一个tick数据对象
	 *	@stdCode 合约代码
	 *	@return 输入为空或代码长度超出目标字段容量时返回NULL
	 */
	static inline WTSTickData* create(const char* stdCode) noexcept
	{
		if (stdCode == NULL)
			return NULL;

		std::size_t len = 0;
		while (len < MAX_INSTRUMENT_LENGTH && stdCode[len] != '\0')
			++len;
		if (len == MAX_INSTRUMENT_LENGTH)
			return NULL;

		WTSTickData* pRet = WTSTickData::allocate();
		if (pRet == NULL)
			return NULL;
		memcpy(pRet->m_tickStruct.code, stdCode, len);
		pRet->m_tickStruct.code[len] = 0;

		return pRet;
	}

	/*
	 *	根据tick结构体创建一个tick数据对象
	 *	@tickData tick结构体
	 */
	static inline WTSTickData* create(WTSTickStruct& tickData) noexcept
	{
		WTSTickData* pRet = allocate();
		memcpy(&pRet->m_tickStruct, &tickData, sizeof(WTSTickStruct));

		return pRet;
	}

	/*
	 *	安全设置合约代码。长度为0时按C字符串读取；失败时保留原代码。
	 */
	inline bool trySetCode(const char* code, std::size_t len = 0) noexcept
	{
		if (code == NULL)
			return false;

		const std::size_t capacity = sizeof(m_tickStruct.code);
		if (len == 0)
		{
			while (len < capacity && code[len] != '\0')
				++len;
			if (len == capacity)
				return false;
		}
		else if (len >= capacity)
		{
			return false;
		}

		memmove(m_tickStruct.code, code, len);
		m_tickStruct.code[len] = '\0';
		return true;
	}

	/* 保留旧接口；需要处理拒绝结果的调用方应使用trySetCode。 */
	inline void setCode(const char* code, std::size_t len = 0) noexcept
	{
		(void)trySetCode(code, len);
	}

	/*
	 *	读取合约代码
	 */
	constexpr inline const char* code() const noexcept { return m_tickStruct.code; }

	/*
	 *	读取市场代码
	 */
	constexpr inline const char*	exchg() const noexcept { return m_tickStruct.exchg; }

	/*
	 *	读取最新价
	 */
	constexpr inline double	price() const noexcept { return m_tickStruct.price; }

	constexpr inline double	open() const noexcept { return m_tickStruct.open; }

	/*
	 *	最高价
	 */
	constexpr inline double	high() const noexcept { return m_tickStruct.high; }

	/*
	 *	最低价
	 */
	constexpr inline double	low() const noexcept { return m_tickStruct.low; }

	//昨收价,如果是期货则是昨结算
	constexpr inline double	preclose() const noexcept { return m_tickStruct.pre_close; }
	constexpr inline double	presettle() const noexcept { return m_tickStruct.pre_settle; }
	constexpr inline double	preinterest() const noexcept { return m_tickStruct.pre_interest; }

	constexpr inline double	upperlimit() const noexcept { return m_tickStruct.upper_limit; }
	constexpr inline double	lowerlimit() const noexcept { return m_tickStruct.lower_limit; }
	//成交量
	constexpr inline double	totalvolume() const noexcept { return m_tickStruct.total_volume; }

	//成交量
	constexpr inline double	volume() const noexcept { return m_tickStruct.volume; }

	//结算价
	constexpr inline double	settlepx() const noexcept { return m_tickStruct.settle_price; }

	//总持
	constexpr inline double	openinterest() const noexcept { return m_tickStruct.open_interest; }

	constexpr inline double	additional() const noexcept { return m_tickStruct.diff_interest; }

	//成交额
	constexpr inline double	totalturnover() const noexcept { return m_tickStruct.total_turnover; }

	//成交额
	constexpr inline double	turnover() const noexcept { return m_tickStruct.turn_over; }

	//交易日
	constexpr inline uint32_t	tradingdate() const noexcept { return m_tickStruct.trading_date; }

	//数据发生日期
	constexpr inline uint32_t	actiondate() const noexcept { return m_tickStruct.action_date; }

	//数据发生时间
	constexpr inline uint32_t	actiontime() const noexcept { return m_tickStruct.action_time; }


	/*
	 *	读取指定档位的委买价
	 *	@idx 0-9
	 */
	constexpr inline double		bidprice(int idx) const noexcept
	{
		if(idx < 0 || idx >= 10) 
			return -1;

		return m_tickStruct.bid_prices[idx];
	}

	/*
	 *	读取指定档位的委卖价
	 *	@idx 0-9
	 */
	constexpr inline double		askprice(int idx) const noexcept
	{
		if(idx < 0 || idx >= 10) 
			return -1;

		return m_tickStruct.ask_prices[idx];
	}

	/*
	 *	读取指定档位的委买量
	 *	@idx 0-9
	 */
	constexpr inline double	bidqty(int idx) const noexcept
	{
		if(idx < 0 || idx >= 10) 
			return -1;

		return m_tickStruct.bid_qty[idx];
	}

	/*
	 *	读取指定档位的委卖量
	 *	@idx 0-9
	 */
	constexpr inline double	askqty(int idx) const noexcept
	{
		if(idx < 0 || idx >= 10) 
			return -1;

		return m_tickStruct.ask_qty[idx];
	}

	/*
	 *	返回tick结构体的引用
	 */
	constexpr inline WTSTickStruct&	getTickStruct() noexcept{ return m_tickStruct; }

	constexpr inline void setContractInfo(WTSContractInfo* cInfo) noexcept { m_pContract = cInfo; }
	constexpr inline WTSContractInfo* getContractInfo() const noexcept { return m_pContract; }

	inline void setLimitFlag(uint32_t flag) { m_uLimitFlag = flag; }
	constexpr inline uint32_t getLimitFlag() const { return m_uLimitFlag; }

	constexpr inline bool isNewHigh() const { return m_uLimitFlag & 1; }
	constexpr inline bool isNewLow() const { return m_uLimitFlag & 2; }

private:
	WTSTickStruct		m_tickStruct;
	WTSContractInfo*	m_pContract;
	uint32_t			m_uLimitFlag = 0;
};

/*
 *	By Wesley @ 2024.04.09
 *	Tick数据引用
 *	该类直接引用已经分配好的WTSTickStruct指针
 *	而不再自己维护一个拷贝副本
 *	优点：开销更小
 *	缺点：如果分配的内存释放了，引用就会成为野指针
 *	如果使用者要在本地retain，已经要注意这个问题
 *	推荐只在极短的时间内在本地维护
 */
class WTSTickRef : public WTSPoolObject<WTSTickRef>
{	
	friend WTSPoolObject<WTSTickRef>::PoolType;
protected:
	WTSTickRef() : m_tickStruct(nullptr) ,m_pContract(nullptr){}

public:
	/*
	 *	创建一个tick数据对象
	 *	@stdCode 合约代码
	 */
	static inline WTSTickRef* create(WTSTickStruct* tickStruct, WTSContractInfo* cInfo = nullptr) noexcept
	{
		WTSTickRef* pRet = WTSTickRef::allocate();
		pRet->m_tickStruct = tickStruct;
		pRet->m_pContract = cInfo;

		return pRet;
	}

	/*
	 *	读取合约代码
	 */
	constexpr inline const char* code() const noexcept { return m_tickStruct->code; }

	/*
	 *	读取市场代码
	 */
	constexpr inline const char*	exchg() const noexcept { return m_tickStruct->exchg; }

	/*
	 *	读取最新价
	 */
	constexpr inline double	price() const noexcept { return m_tickStruct->price; }

	constexpr inline double	open() const noexcept { return m_tickStruct->open; }

	/*
	 *	最高价
	 */
	constexpr inline double	high() const noexcept { return m_tickStruct->high; }

	/*
	 *	最低价
	 */
	constexpr inline double	low() const noexcept { return m_tickStruct->low; }

	//昨收价,如果是期货则是昨结算
	constexpr inline double	preclose() const noexcept { return m_tickStruct->pre_close; }
	constexpr inline double	presettle() const noexcept { return m_tickStruct->pre_settle; }
	constexpr inline double	preinterest() const noexcept { return m_tickStruct->pre_interest; }

	constexpr inline double	upperlimit() const noexcept { return m_tickStruct->upper_limit; }
	constexpr inline double	lowerlimit() const noexcept { return m_tickStruct->lower_limit; }
	//成交量
	constexpr inline double	totalvolume() const noexcept { return m_tickStruct->total_volume; }

	//成交量
	constexpr inline double	volume() const noexcept { return m_tickStruct->volume; }

	//结算价
	constexpr inline double	settlepx() const noexcept { return m_tickStruct->settle_price; }

	//总持
	constexpr inline double	openinterest() const noexcept { return m_tickStruct->open_interest; }

	constexpr inline double	additional() const noexcept { return m_tickStruct->diff_interest; }

	//成交额
	constexpr inline double	totalturnover() const noexcept { return m_tickStruct->total_turnover; }

	//成交额
	constexpr inline double	turnover() const noexcept { return m_tickStruct->turn_over; }

	//交易日
	constexpr inline uint32_t	tradingdate() const noexcept { return m_tickStruct->trading_date; }

	//数据发生日期
	constexpr inline uint32_t	actiondate() const noexcept { return m_tickStruct->action_date; }

	//数据发生时间
	constexpr inline uint32_t	actiontime() const noexcept { return m_tickStruct->action_time; }


	/*
	 *	读取指定档位的委买价
	 *	@idx 0-9
	 */
	constexpr inline double		bidprice(int idx) const noexcept
	{
		if (idx < 0 || idx >= 10)
			return -1;

		return m_tickStruct->bid_prices[idx];
	}

	/*
	 *	读取指定档位的委卖价
	 *	@idx 0-9
	 */
	constexpr inline double		askprice(int idx) const noexcept
	{
		if (idx < 0 || idx >= 10)
			return -1;

		return m_tickStruct->ask_prices[idx];
	}

	/*
	 *	读取指定档位的委买量
	 *	@idx 0-9
	 */
	constexpr inline double	bidqty(int idx) const noexcept
	{
		if (idx < 0 || idx >= 10)
			return -1;

		return m_tickStruct->bid_qty[idx];
	}

	/*
	 *	读取指定档位的委卖量
	 *	@idx 0-9
	 */
	constexpr inline double	askqty(int idx) const noexcept
	{
		if (idx < 0 || idx >= 10)
			return -1;

		return m_tickStruct->ask_qty[idx];
	}

	constexpr inline bool valid() const noexcept { return m_tickStruct != nullptr; }

	constexpr inline operator bool() const noexcept { return m_tickStruct != nullptr; }

	/*
	 *	返回tick结构体的引用
	 */
	constexpr inline WTSTickStruct*	getTickStruct() noexcept { return m_tickStruct; }

	constexpr inline void setContractInfo(WTSContractInfo* cInfo) noexcept { m_pContract = cInfo; }
	constexpr inline WTSContractInfo* getContractInfo() const noexcept { return m_pContract; }

private:
	WTSTickStruct*		m_tickStruct = nullptr;
	WTSContractInfo*	m_pContract = nullptr;
};


class WTSOrdQueData : public WTSPoolObject< WTSOrdQueData>
{
public:
	static inline WTSOrdQueData* create(const char* code) noexcept
	{
		WTSOrdQueData* pRet = WTSOrdQueData::allocate();
		wt_strcpy(pRet->m_oqStruct.code, code);
		return pRet;
	}

	static inline WTSOrdQueData* create(WTSOrdQueStruct& ordQueData) noexcept
	{
		WTSOrdQueData* pRet = WTSOrdQueData::allocate();
		memcpy(&pRet->m_oqStruct, &ordQueData, sizeof(WTSOrdQueStruct));

		return pRet;
	}

	constexpr inline WTSOrdQueStruct& getOrdQueStruct(){return m_oqStruct;}

	inline const char* exchg() const{ return m_oqStruct.exchg; }
	inline const char* code() const{ return m_oqStruct.code; }
	inline uint32_t tradingdate() const{ return m_oqStruct.trading_date; }
	inline uint32_t actiondate() const{ return m_oqStruct.action_date; }
	inline uint32_t actiontime() const { return m_oqStruct.action_time; }

	inline void		setCode(const char* code) { wt_strcpy(m_oqStruct.code, code); }

	constexpr inline void setContractInfo(WTSContractInfo* cInfo) noexcept { m_pContract = cInfo; }
	constexpr inline WTSContractInfo* getContractInfo() const noexcept{ return m_pContract; }

private:
	WTSOrdQueStruct		m_oqStruct;
	WTSContractInfo*	m_pContract;
};

class WTSOrdDtlData : public WTSPoolObject<WTSOrdDtlData>
{
public:
	static inline WTSOrdDtlData* create(const char* code) noexcept
	{
		WTSOrdDtlData* pRet = WTSOrdDtlData::allocate();
		wt_strcpy(pRet->m_odStruct.code, code);
		return pRet;
	}

	static inline WTSOrdDtlData* create(WTSOrdDtlStruct& odData) noexcept
	{
		WTSOrdDtlData* pRet = WTSOrdDtlData::allocate();
		memcpy(&pRet->m_odStruct, &odData, sizeof(WTSOrdDtlStruct));

		return pRet;
	}

	inline WTSOrdDtlStruct& getOrdDtlStruct(){ return m_odStruct; }

	inline const char* exchg() const{ return m_odStruct.exchg; }
	inline const char* code() const{ return m_odStruct.code; }
	inline uint32_t tradingdate() const{ return m_odStruct.trading_date; }
	inline uint32_t actiondate() const{ return m_odStruct.action_date; }
	inline uint32_t actiontime() const { return m_odStruct.action_time; }

	inline void		setCode(const char* code) { wt_strcpy(m_odStruct.code, code); }

	inline void setContractInfo(WTSContractInfo* cInfo) { m_pContract = cInfo; }
	inline WTSContractInfo* getContractInfo() const { return m_pContract; }


private:
	WTSOrdDtlStruct		m_odStruct;
	WTSContractInfo*	m_pContract;
};

class WTSTransData : public WTSPoolObject<WTSTransData>
{
public:
	static inline WTSTransData* create(const char* code) noexcept
	{
		WTSTransData* pRet = WTSTransData::allocate();
		wt_strcpy(pRet->m_tsStruct.code, code);
		return pRet;
	}

	static inline WTSTransData* create(WTSTransStruct& transData) noexcept
	{
		WTSTransData* pRet = WTSTransData::allocate();
		memcpy(&pRet->m_tsStruct, &transData, sizeof(WTSTransStruct));

		return pRet;
	}

	inline const char* exchg() const{ return m_tsStruct.exchg; }
	inline const char* code() const{ return m_tsStruct.code; }
	inline uint32_t tradingdate() const{ return m_tsStruct.trading_date; }
	inline uint32_t actiondate() const{ return m_tsStruct.action_date; }
	inline uint32_t actiontime() const { return m_tsStruct.action_time; }

	inline WTSTransStruct& getTransStruct(){ return m_tsStruct; }

	inline void		setCode(const char* code) { wt_strcpy(m_tsStruct.code, code); }

	inline void setContractInfo(WTSContractInfo* cInfo) { m_pContract = cInfo; }
	inline WTSContractInfo* getContractInfo() const { return m_pContract; }

private:
	WTSTransStruct		m_tsStruct;
	WTSContractInfo*	m_pContract;
};

/*
 *	@brief 历史Tick数据数组
 *	@details 内部使用WTSArray作为容器
 */
class WTSHisTickData : public WTSObject
{
protected:
	char						m_strCode[32];
	std::vector<WTSTickStruct>	m_ayTicks;
	bool						m_bValidOnly;
	double						m_dFactor;

	WTSHisTickData() :m_bValidOnly(false), m_dFactor(1.0){}

public:
	/*
	 *	@brief 创建指定大小的tick数组对象
	 *	@details 内部的数组预先分配大小
	 *
	 *	@param stdCode 合约代码
	 *	@param nSize 预先分配的大小
	 */
	static inline WTSHisTickData* create(const char* stdCode, unsigned int nSize, bool bValidOnly = false, double factor = 1.0)
	{
		WTSHisTickData *pRet = new WTSHisTickData;
		wt_strcpy(pRet->m_strCode, stdCode);
		pRet->m_ayTicks.resize(nSize);
		pRet->m_bValidOnly = bValidOnly;
		pRet->m_dFactor = factor;

		return pRet;
	}

	/*
	 *	@brief 根据tick数组对象创建历史tick数据对象
	 *	@details 内部的tick数组不用再分配了

	 *	@param ayTicks tick数组对象指针
	 */
	static inline WTSHisTickData* create(const char* stdCode, bool bValidOnly = false, double factor = 1.0)
	{
		WTSHisTickData *pRet = new WTSHisTickData;
		wt_strcpy(pRet->m_strCode, stdCode);
		pRet->m_bValidOnly = bValidOnly;
		pRet->m_dFactor = factor;

		return pRet;
	}

	//读取tick数据的条数
	inline uint32_t	size() const{ return m_ayTicks.size(); }
	inline bool		empty() const{ return m_ayTicks.empty(); }

	//读取该数据对应的合约代码
	inline const char*		code() const{ return m_strCode; }

	/*
	 *	获取指定位置的tick数据
	 *	
	 */
	inline WTSTickStruct*	at(uint32_t idx)
	{
		if (m_ayTicks.empty() || idx >= m_ayTicks.size())
			return NULL;

		return &m_ayTicks[idx];
	}

	inline std::vector<WTSTickStruct>& getDataRef() { return m_ayTicks; }

	inline bool isValidOnly() const{ return m_bValidOnly; }

	//复权因子（卡11=B：写时复制克隆缓存时需要保持一致）
	inline double factor() const{ return m_dFactor; }

	/*
	*	追加一条Tick
	*/
	inline void	appendTick(const WTSTickStruct& ts)
	{
		m_ayTicks.emplace_back(ts);
		//复权修正
		m_ayTicks.back().price *= m_dFactor;
		m_ayTicks.back().open *= m_dFactor;
		m_ayTicks.back().high *= m_dFactor;
		m_ayTicks.back().low *= m_dFactor;
	}
};

//////////////////////////////////////////////////////////////////////////
/*
 *	@brief Tick数据切片,从连续的tick缓存中做的切片
 *	@details 切片并没有真实的复制内存,而只是取了开始和结尾的下标
 *	这样使用虽然更快,但是使用场景要非常小心,因为他依赖于基础数据对象
 */
class WTSTickSlice : public WTSObject
{
private:
	char			_code[MAX_INSTRUMENT_LENGTH];
	typedef std::pair<WTSTickStruct*, uint32_t> TickBlock;
	std::vector<TickBlock> _blocks;
	uint32_t		_count;
	WTSSliceOwners	_owners;	//追加成员，保持既有成员偏移不变

protected:
	WTSTickSlice() : _count(0) { _blocks.clear(); }
	inline int32_t		translateIdx(int32_t idx) const
	{
		if (idx < 0)
		{
			return max(0, (int32_t)_count + idx);
		}

		return idx;
	}

public:
	static inline WTSTickSlice* create(const char* code, WTSTickStruct* ticks = NULL, uint32_t count = 0)
	{
		//if (ticks == NULL || count == 0)
		//	return NULL;
		// I03-NEW-083: same contract as WTSTickData::create (NULL or overlong code -> NULL).
		if (code == NULL)
			return NULL;
		std::size_t len = 0;
		while (len < MAX_INSTRUMENT_LENGTH && code[len] != '\0')
			++len;
		if (len == MAX_INSTRUMENT_LENGTH)
			return NULL;

		WTSTickSlice* slice = new WTSTickSlice();
		memcpy(slice->_code, code, len);
		slice->_code[len] = '\0';
		if(ticks != NULL)
		{
			slice->_blocks.emplace_back(TickBlock(ticks, count));
			slice->_count = count;
		}

		return slice;
	}

	inline bool appendBlock(WTSTickStruct* ticks, uint32_t count)
	{
		if (ticks == NULL || count == 0)
			return false;

		_count += count;
		_blocks.emplace_back(TickBlock(ticks, count));
		return true;
	}

	inline bool insertBlock(std::size_t idx, WTSTickStruct* ticks, uint32_t count)
	{
		if (ticks == NULL || count == 0)
			return false;

		_count += count;
		_blocks.insert(_blocks.begin()+idx, TickBlock(ticks, count));
		return true;
	}

	inline std::size_t	get_block_counts() const
	{
		return _blocks.size();
	}

	inline WTSTickStruct*	get_block_addr(std::size_t blkIdx)
	{
		if (blkIdx >= _blocks.size())
			return NULL;

		return _blocks[blkIdx].first;
	}

	inline uint32_t get_block_size(std::size_t blkIdx)
	{
		if (blkIdx >= _blocks.size())
			return INVALID_UINT32;

		return _blocks[blkIdx].second;
	}

	inline uint32_t size() const{ return _count; }

	inline bool empty() const{ return (_count == 0); }

	/*
	 *	登记底层缓冲区 owner（卡11=B）：切片释放前 owner 不会被销毁
	 */
	inline bool addOwner(const WTSSliceOwnerPtr& owner) { return _owners.add(owner); }
	inline std::size_t ownerCount() const { return _owners.count(); }

	inline const WTSTickStruct* at(int32_t idx)
	{
		if (_count == 0)
			return NULL;

		idx = translateIdx(idx);
		do 
		{
			for(auto& item : _blocks)
			{
				if ((uint32_t)idx >= item.second)
					idx -= item.second;
				else
					return item.first + idx;
			}
		} while (false);
		return NULL;
	}
};

//////////////////////////////////////////////////////////////////////////
/*
 *	@brief 逐笔委托数据切片,从连续的逐笔委托缓存中做的切片
 *	@details 切片并没有真实的复制内存,而只是取了开始和结尾的下标
 *	这样使用虽然更快,但是使用场景要非常小心,因为他依赖于基础数据对象
 */
class WTSOrdDtlSlice : public WTSObject
{
private:
	char				m_strCode[MAX_INSTRUMENT_LENGTH];
	WTSOrdDtlStruct*	m_ptrBegin;
	uint32_t			m_uCount;
	WTSSliceOwners		m_owners;	//追加成员，保持既有成员偏移不变

protected:
	WTSOrdDtlSlice() :m_ptrBegin(NULL), m_uCount(0) {}
	inline int32_t		translateIdx(int32_t idx) const
	{
		if (idx < 0)
		{
			return max(0, (int32_t)m_uCount + idx);
		}

		return idx;
	}

public:
	static inline WTSOrdDtlSlice* create(const char* code, WTSOrdDtlStruct* firstItem, uint32_t count)
	{
		if (count == 0 || firstItem == NULL)
			return NULL;

		WTSOrdDtlSlice* slice = new WTSOrdDtlSlice();
		wt_strcpy(slice->m_strCode, code);
		slice->m_ptrBegin = firstItem;
		slice->m_uCount = count;

		return slice;
	}

	inline uint32_t size() const { return m_uCount; }

	inline bool empty() const { return (m_uCount == 0) || (m_ptrBegin == NULL); }

	/*
	 *	登记底层缓冲区 owner（卡11=B）：切片释放前 owner 不会被销毁
	 */
	inline bool addOwner(const WTSSliceOwnerPtr& owner) { return m_owners.add(owner); }
	inline std::size_t ownerCount() const { return m_owners.count(); }

	inline const WTSOrdDtlStruct* at(int32_t idx)
	{
		if (m_ptrBegin == NULL)
			return NULL;
		idx = translateIdx(idx);
		// I03-NEW-092: an index past the slice end must not yield a pointer outside the buffer.
		if (idx < 0 || static_cast<uint32_t>(idx) >= m_uCount)
			return NULL;
		return m_ptrBegin + idx;
	}
};

//////////////////////////////////////////////////////////////////////////
/*
 *	@brief 委托队列数据切片,从连续的委托队列缓存中做的切片
 *	@details 切片并没有真实的复制内存,而只是取了开始和结尾的下标
 *	这样使用虽然更快,但是使用场景要非常小心,因为他依赖于基础数据对象
 */
class WTSOrdQueSlice : public WTSObject
{
private:
	char				m_strCode[MAX_INSTRUMENT_LENGTH];
	WTSOrdQueStruct*	m_ptrBegin;
	uint32_t			m_uCount;
	WTSSliceOwners		m_owners;	//追加成员，保持既有成员偏移不变

protected:
	WTSOrdQueSlice() :m_ptrBegin(NULL), m_uCount(0) {}
	inline int32_t		translateIdx(int32_t idx) const
	{
		if (idx < 0)
		{
			return max(0, (int32_t)m_uCount + idx);
		}

		return idx;
	}

public:
	static inline WTSOrdQueSlice* create(const char* code, WTSOrdQueStruct* firstItem, uint32_t count)
	{
		if (count == 0 || firstItem == NULL)
			return NULL;

		WTSOrdQueSlice* slice = new WTSOrdQueSlice();
		wt_strcpy(slice->m_strCode, code);
		slice->m_ptrBegin = firstItem;
		slice->m_uCount = count;

		return slice;
	}

	inline uint32_t size() const { return m_uCount; }

	inline bool empty() const { return (m_uCount == 0) || (m_ptrBegin == NULL); }

	/*
	 *	登记底层缓冲区 owner（卡11=B）：切片释放前 owner 不会被销毁
	 */
	inline bool addOwner(const WTSSliceOwnerPtr& owner) { return m_owners.add(owner); }
	inline std::size_t ownerCount() const { return m_owners.count(); }

	inline const WTSOrdQueStruct* at(int32_t idx)
	{
		if (m_ptrBegin == NULL)
			return NULL;
		idx = translateIdx(idx);
		// I03-NEW-092: an index past the slice end must not yield a pointer outside the buffer.
		if (idx < 0 || static_cast<uint32_t>(idx) >= m_uCount)
			return NULL;
		return m_ptrBegin + idx;
	}
};

//////////////////////////////////////////////////////////////////////////
/*
 *	@brief 逐笔成交数据切片,从连续的逐笔成交缓存中做的切片
 *	@details 切片并没有真实的复制内存,而只是取了开始和结尾的下标
 *	这样使用虽然更快,但是使用场景要非常小心,因为他依赖于基础数据对象
 */
class WTSTransSlice : public WTSObject
{
private:
	char			m_strCode[MAX_INSTRUMENT_LENGTH];
	WTSTransStruct*	m_ptrBegin;
	uint32_t		m_uCount;
	WTSSliceOwners	m_owners;	//追加成员，保持既有成员偏移不变

protected:
	WTSTransSlice() :m_ptrBegin(NULL), m_uCount(0) {}
	inline int32_t		translateIdx(int32_t idx) const
	{
		if (idx < 0)
		{
			return max(0, (int32_t)m_uCount + idx);
		}

		return idx;
	}

public:
	static inline WTSTransSlice* create(const char* code, WTSTransStruct* firstItem, uint32_t count)
	{
		if (count == 0 || firstItem == NULL)
			return NULL;

		WTSTransSlice* slice = new WTSTransSlice();
		wt_strcpy(slice->m_strCode, code);
		slice->m_ptrBegin = firstItem;
		slice->m_uCount = count;

		return slice;
	}

	inline uint32_t size() const { return m_uCount; }

	inline bool empty() const { return (m_uCount == 0) || (m_ptrBegin == NULL); }

	/*
	 *	登记底层缓冲区 owner（卡11=B）：切片释放前 owner 不会被销毁
	 */
	inline bool addOwner(const WTSSliceOwnerPtr& owner) { return m_owners.add(owner); }
	inline std::size_t ownerCount() const { return m_owners.count(); }

	inline const WTSTransStruct* at(int32_t idx)
	{
		if (m_ptrBegin == NULL)
			return NULL;
		idx = translateIdx(idx);
		// I03-NEW-092: an index past the slice end must not yield a pointer outside the buffer.
		if (idx < 0 || static_cast<uint32_t>(idx) >= m_uCount)
			return NULL;
		return m_ptrBegin + idx;
	}
};

NS_WTP_END
