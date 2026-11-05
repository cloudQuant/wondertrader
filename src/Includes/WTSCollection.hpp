/*!
 * \file WTSCollection.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief Wt集合组件定义文件
 */
#pragma once
#include "WTSObject.hpp"
#include <vector>
#include <map>
#include <functional>
#include <algorithm>
#include "FasterDefs.h"

#include <deque>

/*
 *	容器所有权合同版本（迭代03 卡12，2026-11-03 裁定 12a=A、12b=C）
 *	- WTSMap::erase 三个重载均释放容器持有的那份引用，与 remove/clear 一致
 *	- WTSStack 提供显式 pushOwned/pushTransferred/pushBorrowed；
 *	  旧 push(obj,false) 按所有权转移兼容；pop 始终返回调用方须 release 一次的拥有引用
 *	外部插件可据此宏判断所编译头文件的语义版本
 */
#define WTS_COLLECTION_OWNERSHIP_CONTRACT 20261103L

NS_WTP_BEGIN

//////////////////////////////////////////////////////////////////////////
//WTSArray

/*
 *	平台数组容器
 *	内部使用vector实现
 *	数据使用WTSObject指针对象
 *	所有WTSObject的派生类都可以使用
 *	用于平台内使用
 */
class WTSArray : public WTSObject
{
public:
	/*
	 *	数组迭代器
	 */
	typedef std::vector<WTSObject*>::iterator Iterator;
	typedef std::vector<WTSObject*>::const_iterator ConstIterator;

	typedef std::vector<WTSObject*>::reverse_iterator ReverseIterator;
	typedef std::vector<WTSObject*>::const_reverse_iterator ConstReverseIterator;

	typedef std::function<bool(WTSObject*, WTSObject*)>	SortFunc;

	/*
	 *	创建数组对象
	 */
	static WTSArray* create() noexcept
	{
		WTSArray* pRet = new WTSArray();
		return pRet;
	}

	/*
	 *	读取数组长度
	 */
	inline uint32_t size() const{ return (uint32_t)_vec.size(); }

	inline bool empty() const noexcept { return _vec.empty(); }

	/*
	 *	清空数组,并重新分配空间
	 *	调用该函数会预先分配长度
	 *	预先分配好的数据都是NULL
	 */
	void resize(uint32_t _size, WTSObject* val = NULL) noexcept
	{
		if(!_vec.empty())
			clear();

		_vec.resize(_size, val);
	}

	/*
	 *	读取数组指定位置的数据
	 *	对比grab接口,at接口只取得数据
	 *	不增加数据的引用计数
	 *	grab接口读取数据以后,增加引用计数
	 */
	inline
	WTSObject* at(uint32_t idx) noexcept
	{
		if(idx <0 || idx >= _vec.size())
			return NULL;

		WTSObject* pRet = _vec.at(idx);
		return pRet;
	}

	inline
	uint32_t idxOf(WTSObject* obj) noexcept
	{
		if (obj == NULL)
			return -1;

		uint32_t idx = 0;
		auto it = _vec.begin();
		for (; it != _vec.end(); it++, idx++)
		{
			if (obj == (*it))
				return idx;
		}

		return -1;
	}

	template<typename T> 
	inline T* at(uint32_t idx) noexcept
	{
		if(idx <0 || idx >= _vec.size())
			return NULL;

		WTSObject* pRet = _vec.at(idx);
		return static_cast<T*>(pRet);
	}

	/*
	 *	[]操作符重载
	 *	用法同at函数
	 */
	inline
	WTSObject* operator [](uint32_t idx) noexcept
	{
		if(idx <0 || idx >= _vec.size())
			return NULL;

		WTSObject* pRet = _vec.at(idx);
		return pRet;
	}

	/*
	 *	读取数组指定位置的数据
	 *	增加引用计数
	 */
	inline
	WTSObject*	grab(uint32_t idx) noexcept
	{
		if(idx <0 || idx >= _vec.size())
			return NULL;

		WTSObject* pRet = _vec.at(idx);
		if (pRet)
			pRet->retain();

		return pRet;
	}

	/*
	 *	数组末尾追加数据
	 *	数据自动增加引用计数
	 */
	inline
	void append(WTSObject* obj, bool bAutoRetain = true) noexcept
	{
		if (bAutoRetain && obj)
			obj->retain();

		_vec.emplace_back(obj);
	}

	/*
	 *	设置指定位置的数据
	 *	如果该位置已有数据,则释放掉
	 *	新数据引用计数增加
	 */
	inline
	void set(uint32_t idx, WTSObject* obj, bool bAutoRetain = true) noexcept
	{
		if(idx >= _vec.size())
			return;

		if(bAutoRetain && obj)
			obj->retain();

		//WTSObject* oldObj = _vec.at(idx);
		std::swap(_vec.at(idx), obj);

		if(obj)
			obj->release();
	}

	inline
	void append(WTSArray* ay) noexcept
	{
		if(ay == NULL)
			return;

		_vec.insert(_vec.end(), ay->_vec.begin(), ay->_vec.end());
		ay->_vec.clear();
	}

	/*
	 *	数组清空
	 *	数组内所有数据释放引用
	 */
	void clear() noexcept
	{
		{
			std::vector<WTSObject*>::iterator it = _vec.begin();
			for (; it != _vec.end(); it++)
			{
				WTSObject* obj = (*it);
				if (obj)
					obj->release();
			}
		}
		
		_vec.clear();
	}

	/*
	 *	释放数组对象,用法如WTSObject
	 *	不同的是,如果引用计数为1时
	 *	释放所有数据
	 */

	virtual void release() noexcept
	{
		if (m_uRefs == 0)
			return;

		try
		{
			uint32_t refs = m_uRefs.fetch_sub(1);
			if (refs == 1)
			{
				clear();
				delete this;
			}
		}
		catch(...)
		{

		}
	}

	/*
	 *	取得数组对象起始位置的迭代器
	 */
	inline
	Iterator begin() noexcept
	{
		return _vec.begin();
	}

	inline
	ConstIterator begin() const noexcept
	{
		return _vec.begin();
	}

	inline
	ReverseIterator rbegin() noexcept
	{
		return _vec.rbegin();
	}

	inline
	ConstReverseIterator rbegin() const noexcept
	{
		return _vec.rbegin();
	}

	/*
	 *	取得数组对象末尾位置的迭代器
	 */
	inline
	Iterator end() noexcept
	{
		return _vec.end();
	}

	inline
	ConstIterator end() const noexcept
	{
		return _vec.end();
	}

	inline
	ReverseIterator rend() noexcept
	{
		return _vec.rend();
	}

	inline
	ConstReverseIterator rend() const noexcept
	{
		return _vec.rend();
	}

	inline
	void	sort(SortFunc func)
	{
		std::sort(_vec.begin(), _vec.end(), func);
	}

protected:
	WTSArray():_holding(false){}
	virtual ~WTSArray(){}

	std::vector<WTSObject*>	_vec;
	std::atomic<bool>		_holding;
};


/*
 *	map容器
 *	内部采用std:map实现
 *	模版类型为key类型
 *	数据使用WTSObject指针对象
 *	所有WTSObject的派生类都适用
 */
template <class T>
class WTSMap : public WTSObject
{
public:
	/*
	 *	容器迭代器的定义
	 */
	typedef typename std::map<T, WTSObject*>	_MyType;
	typedef typename _MyType::iterator			Iterator;
	typedef typename _MyType::const_iterator	ConstIterator;
	typedef typename _MyType::reverse_iterator			ReverseIterator;
	typedef typename _MyType::const_reverse_iterator	ConstReverseIterator;

	/*
	 *	创建map容器
	 */
	static WTSMap<T>*	create() noexcept
	{
		WTSMap<T>* pRet = new WTSMap<T>();
		return pRet;
	}

	/*
	 *	返回map容器的大小
	 */
	inline
	uint32_t size() const noexcept { return (uint32_t)_map.size(); }

	/*
	 *	读取指定key对应的数据
	 *	不增加数据的引用计数
	 *	没有或映射值为NULL则返回NULL；需用find区分键是否存在
	 */
	inline
	WTSObject* get(const T &_key) noexcept
	{
		Iterator it = _map.find(_key);
		if(it == _map.end())
			return NULL;

		WTSObject* pRet = it->second;
		return pRet;
	}

	/*
	 *	[]操作符重载
	 *	用法同get函数
	 */
	inline
	WTSObject* operator[](const T &_key) noexcept
	{
		Iterator it = _map.find(_key);
		if(it == _map.end())
			return NULL;

		WTSObject* pRet = it->second;
		return pRet;
	}

	/*
	 *	读取指定key对应的数据
	 *	增加数据的引用计数
	 *	没有或映射值为NULL则返回NULL；需用find区分键是否存在
	 */
	inline
	WTSObject* grab(const T &_key) noexcept
	{
		Iterator it = _map.find(_key);
		if(it == _map.end())
			return NULL;

		WTSObject* pRet = it->second;
		if (pRet)
			pRet->retain();

		return pRet;
	}

	/*
	 *	新增一个数据,并增加数据引用计数
	 *	如果key存在,则将原有数据释放
	 *	允许obj为NULL；键仍保留在容器中并计入size
	 */
	inline
	void add(T _key, WTSObject* obj, bool bAutoRetain = true) noexcept
	{
		if(bAutoRetain && obj)
			obj->retain();

		WTSObject* pOldObj = NULL;
		Iterator it = _map.find(_key);
		if(it != _map.end())
		{
			pOldObj = it->second;
		}

		_map[_key] = obj;

		if (pOldObj) pOldObj->release();
	}

	/*
	 *	根据key删除一个数据
	 *	如果key存在,则对应数据引用计数-1
	 */
	inline
	void remove(T _key) noexcept
	{
		Iterator it = _map.find(_key);
		if(it != _map.end())
		{
			WTSObject* obj = it->second;
			_map.erase(it);
			if (obj) obj->release();
		}
	}

	/*
	 *	获取容器起始位置的迭代器
	 */
	Iterator begin() noexcept
	{
		return _map.begin();
	}

	ConstIterator begin() const noexcept
	{
		return _map.begin();
	}

	/*
	 *	获取容易末尾位置的迭代器
	 */
	Iterator end() noexcept
	{
		return _map.end();
	}

	ConstIterator end() const noexcept
	{
		return _map.end();
	}

	/*
	 *	获取容器起始位置的迭代器
	 */
	ReverseIterator rbegin() noexcept
	{
		return _map.rbegin();
	}

	ConstReverseIterator rbegin() const noexcept
	{
		return _map.rbegin();
	}

	/*
	 *	获取容易末尾位置的迭代器
	 */
	ReverseIterator rend() noexcept
	{
		return _map.rend();
	}

	ConstReverseIterator rend() const noexcept
	{
		return _map.rend();
	}

	inline
	Iterator find(const T& key) noexcept
	{
		return _map.find(key);
	}

	inline
	ConstIterator find(const T& key) const noexcept
	{
		return _map.find(key);
	}

	/*
	 *	按迭代器删除一个数据（it 必须是本容器内有效且不等于 end() 的迭代器）
	 *	与 remove/clear 一致：先摘除节点，再释放容器持有的那份引用（引用计数-1）
	 *	调用方原有的引用不受影响；需要保留对象的调用方须在 erase 前自行 retain
	 */
	inline
	void erase(ConstIterator it) noexcept
	{
		WTSObject* obj = it->second;
		_map.erase(it);
		if (obj) obj->release();
	}

	inline
	void erase(Iterator it) noexcept
	{
		WTSObject* obj = it->second;
		_map.erase(it);
		if (obj) obj->release();
	}

	/*
	 *	按key删除一个数据；key不存在时无操作
	 *	与 remove 语义相同：释放容器持有的那份引用
	 */
	inline
	void erase(T key) noexcept
	{
		Iterator it = _map.find(key);
		if (it == _map.end())
			return;

		erase(it);
	}

	Iterator lower_bound(const T& key) noexcept
	{
		 return _map.lower_bound(key);
	}

	ConstIterator lower_bound(const T& key) const noexcept
	{
		return _map.lower_bound(key);
	}

	Iterator upper_bound(const T& key) noexcept
	{
	 	 return _map.upper_bound(key);
	}
	 
	ConstIterator upper_bound(const T& key) const noexcept
	{
		return _map.upper_bound(key);
	}

	inline
	WTSObject* last() noexcept
	{
		if(_map.empty())
			return NULL;
		
		return _map.rbegin()->second;
	}
	

	/*
	 *	清空容器
	 *	容器内所有数据引用计数-1
	 */
	void clear() noexcept
	{
		Iterator it = _map.begin();
		for(; it != _map.end(); it++)
		{
			if (it->second)
				it->second->release();
		}
		_map.clear();
	}

	/*
	 *	释放容器对象
	 *	如果容器引用计数为1,则清空所有数据
	 */
	virtual void release() noexcept
	{
		if (m_uRefs == 0)
			return;

		try
		{
			uint32_t refs = m_uRefs.fetch_sub(1);
			if (refs == 1)
			{
				clear();
				delete this;
			}
		}
		catch(...)
		{

		}
	}

protected:
	WTSMap(){}
	~WTSMap(){}

	std::map<T, WTSObject*>	_map;
};

/*
 *	map容器
 *	内部采用std:map实现
 *	模版类型为key类型
 *	数据使用WTSObject指针对象
 *	所有WTSObject的派生类都适用
 */
template <typename T, class Hash = std::hash<T>>
class WTSHashMap : public WTSObject
{
protected:
	WTSHashMap() {}
	virtual ~WTSHashMap() {}

	//std::unordered_map<T, WTSObject*>	_map;
	wt_hashmap<T, WTSObject*, Hash>	_map;

public:
	/*
	 *	容器迭代器的定义
	 */
	typedef wt_hashmap<T, WTSObject*, Hash>		_MyType;
	typedef typename _MyType::const_iterator	ConstIterator;

	/*
	 *	创建map容器
	 */
	static WTSHashMap<T, Hash>*	create() noexcept
	{
		WTSHashMap<T, Hash>* pRet = new WTSHashMap<T, Hash>();
		return pRet;
	}

	/*
	 *	返回map容器的大小
	 */
	inline uint32_t size() const{return (uint32_t)_map.size();}

	/*
	 *	读取指定key对应的数据
	 *	不增加数据的引用计数
	 *	没有或映射值为NULL则返回NULL；需用find区分键是否存在
	 */
	inline WTSObject* get(const T &_key) noexcept
	{
		auto it = _map.find(_key);
		if(it == _map.end())
			return NULL;

		WTSObject* pRet = it->second;
		return pRet;
	}

	/*
	 *	读取指定key对应的数据
	 *	增加数据的引用计数
	 *	没有或映射值为NULL则返回NULL；需用find区分键是否存在
	 */
	inline WTSObject* grab(const T &_key) noexcept
	{
		auto it = _map.find(_key);
		if(it == _map.end())
			return NULL;

		WTSObject* pRet = it->second;
		if (pRet)
			pRet->retain();
		return pRet;
	}

	/*
	 *	新增一个数据,并增加数据引用计数
	 *	如果key存在,则将原有数据释放
	 *	允许obj为NULL；键仍保留在容器中并计入size
	 */
	inline void add(const T &_key, WTSObject* obj, bool bAutoRetain = true) noexcept
	{
		if (bAutoRetain && obj)
			obj->retain();

		WTSObject* pOldObj = NULL;
		auto it = _map.find(_key);
		if (it != _map.end())
		{
			pOldObj = it->second;
		}

		_map[_key] = obj;

		if (pOldObj) pOldObj->release();
	}

	/*
	 *	根据key删除一个数据
	 *	如果key存在,则对应数据引用计数-1
	 */
	inline void remove(const T &_key) noexcept
	{
		auto it = _map.find(_key);
		if (it != _map.end())
		{
			WTSObject* obj = it->second;
			_map.erase(it);
			if (obj) obj->release();
		}
	}


	/*
	 *	获取容器起始位置的迭代器
	 */
	inline ConstIterator begin() const noexcept
	{
		return _map.begin();
	}

	/*
	 *	获取容易末尾位置的迭代器
	 */
	inline ConstIterator end() const noexcept
	{
		return _map.end();
	}

	inline ConstIterator find(const T& key) const noexcept
	{
		return _map.find(key);
	}

	/*
	 *	清空容器
	 *	容器内所有数据引用计数-1
	 */
	inline void clear() noexcept
	{
		ConstIterator it = _map.begin();
		for(; it != _map.end(); it++)
		{
			if (it->second)
				it->second->release();
		}
		_map.clear();
	}

	/*
	 *	释放容器对象
	 *	如果容器引用计数为1,则清空所有数据
	 */
	virtual void release() noexcept
	{
		if (m_uRefs == 0)
			return;

		try
		{
			uint32_t refs = m_uRefs.fetch_sub(1);
			if (refs == 1)
			{
				clear();
				delete this;
			}
		}
		catch (...)
		{

		}
	}
};

//////////////////////////////////////////////////////////////////////////
//WTSQueue
class WTSQueue : public WTSObject
{
public:
	typedef std::deque<WTSObject*>::iterator Iterator;
	typedef std::deque<WTSObject*>::const_iterator ConstIterator;

	static WTSQueue* create() noexcept
	{
		WTSQueue* pRet = new WTSQueue();
		return pRet;
	}

	inline void pop() noexcept
	{
		if (_queue.empty())
			return;

		WTSObject* obj = _queue.front();
		_queue.pop_front();
		if (obj)
			obj->release();
	}

	inline void push(WTSObject* obj, bool bAutoRetain = true) noexcept
	{
		if (obj && bAutoRetain)
			obj->retain();

		_queue.emplace_back(obj);
	}

	inline WTSObject* front(bool bRetain = true) noexcept
	{
		if(_queue.empty())
			return NULL;

		WTSObject* obj = _queue.front();
		if (obj && bRetain)
			obj->retain();

		return obj;
	}

	inline WTSObject* back(bool bRetain = true) noexcept
	{
		if(_queue.empty())
			return NULL;

		WTSObject* obj = _queue.back();
		if (obj && bRetain)
			obj->retain();

		return obj;
	}

	inline uint32_t size() const{ return (uint32_t)_queue.size(); }

	inline bool	empty() const{return _queue.empty();}

	void release() noexcept
	{
		if (m_uRefs == 0)
			return;

		try
		{
			uint32_t refs = m_uRefs.fetch_sub(1);
			if (refs == 1)
			{
				clear();
				delete this;
			}
		}
		catch (...)
		{

		}
	}

	void clear() noexcept
	{
		Iterator it = begin();
		for(; it != end(); it++)
		{
			if (*it)
				(*it)->release();
		}
		_queue.clear();
	}

	/*
	 *	取得数组对象起始位置的迭代器
	 */
	inline Iterator begin() noexcept
	{
		return _queue.begin();
	}

	inline ConstIterator begin() const noexcept
	{
		return _queue.begin();
	}

	void swap(WTSQueue* right) noexcept
	{
		_queue.swap(right->_queue);
	}

	/*
	 *	取得数组对象末尾位置的迭代器
	 */
	inline Iterator end() noexcept
	{
		return _queue.end();
	}

	inline ConstIterator end() const noexcept
	{
		return _queue.end();
	}

protected:
	WTSQueue(){}
	virtual ~WTSQueue(){}

	std::deque<WTSObject*>	_queue;
};

/*
 *	栈容器（所有权合同见 WTS_COLLECTION_OWNERSHIP_CONTRACT）
 *	- pushOwned(obj)       ：栈 retain 一份自己的引用，调用方原引用不变
 *	- pushTransferred(obj) ：调用方把自己已有的一份引用转交给栈，栈不 retain
 *	- pushBorrowed(obj)    ：栈只借用、不持有引用；调用方须保证对象在栈内期间存活，
 *	                         clear/析构不会释放借用元素
 *	- push(obj, true)      ：旧入口，等同 pushOwned
 *	- push(obj, false)     ：旧入口，按所有权转移兼容，等同 pushTransferred
 *	- pop()                ：始终返回一份调用方拥有、必须 release 一次的引用；
 *	                         对拥有元素直接交出栈持有的引用，对借用元素先 retain 再返回；
 *	                         空栈或 nullptr 元素返回 nullptr
 *	- top()                ：返回借用指针，不改变引用计数
 *	- clear()/析构         ：只释放栈实际拥有的引用
 */
class WTSStack : public WTSObject
{
public:
	static WTSStack* create() noexcept
	{
		WTSStack* pRet = new WTSStack();
		return pRet;
	}

	inline uint32_t size() const noexcept { return (uint32_t)_vec.size(); }
	inline bool empty() const noexcept { return _vec.empty(); }

	/*
	 *	旧入口：bAutoRetain=true 等同 pushOwned；false 等同 pushTransferred（所有权转移）
	 */
	inline void push(WTSObject* obj, bool bAutoRetain = true) noexcept
	{
		if (bAutoRetain)
			pushOwned(obj);
		else
			pushTransferred(obj);
	}

	inline void pushOwned(WTSObject* obj) noexcept
	{
		if (obj)
			obj->retain();
		_vec.emplace_back(Entry{ obj, true });
	}

	inline void pushTransferred(WTSObject* obj) noexcept
	{
		_vec.emplace_back(Entry{ obj, true });
	}

	inline void pushBorrowed(WTSObject* obj) noexcept
	{
		_vec.emplace_back(Entry{ obj, false });
	}

	/*
	 *	栈顶元素是否为借用元素；空栈返回false
	 */
	inline bool isTopBorrowed() const noexcept
	{
		return !_vec.empty() && !_vec.back()._owned;
	}

	inline WTSObject* pop() noexcept
	{
		if (_vec.empty())
			return nullptr;

		Entry e = _vec.back();
		_vec.pop_back();
		if (e._obj && !e._owned)
			e._obj->retain();
		return e._obj;
	}

	inline WTSObject* top() noexcept
	{
		if (_vec.empty())
			return nullptr;
		return _vec.back()._obj;
	}

	void clear() noexcept
	{
		std::vector<Entry> items;
		items.swap(_vec);
		for (auto& e : items)
		{
			if (e._obj && e._owned)
				e._obj->release();
		}
	}

	virtual void release() noexcept override
	{
		WTSObject::release();
	}

protected:
	WTSStack() = default;
	virtual ~WTSStack()
	{
		clear();
	}

private:
	struct Entry
	{
		WTSObject*	_obj;
		bool		_owned;
	};
	std::vector<Entry> _vec;
};

NS_WTP_END
