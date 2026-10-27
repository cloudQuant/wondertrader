/*!
 * @file ObjectPool.hpp
 * @brief 对象池实现
 * 
 * 该文件实现了一个通用的对象池，用于高效地管理对象的内存分配和释放。
 * 基于boost::pool实现，减少动态内存分配的开销，提高程序性能。
 */

#pragma once
#include <boost/pool/pool.hpp>
#include <atomic>

/**
 * @brief 对象池模板类
 * 
 * @tparam T 池中管理的对象类型
 * 
 * 该类提供对象的快速分配和回收功能，避免频繁的内存分配和释放操作，
 * 特别适用于需要频繁创建和销毁同类型对象的场景。
 */
template < typename T>
class ObjectPool
{
	/**
	 * @brief boost对象池实例
	 * 
	 * 用于管理实际的内存分配与回收
	 */
	boost::pool<> _pool;

public:
	/**
	 * @brief 构造函数
	 * 
	 * 初始化一个对象池，设置分配的内存块大小为T类型的大小
	 */
	ObjectPool() :_pool(sizeof(T)) {}
	
	/**
	 * @brief 虚析构函数
	 * 
	 * 确保子类析构时能够正确调用
	 */
	virtual ~ObjectPool() {}

	/**
	 * @brief 构造一个新对象
	 * 
	 * @return T* 返回新创建的对象指针，如内存分配失败则返回nullptr
	 * 
	 * 从对象池中分配内存，并在该内存上构造一个T类型的对象（使用placement new）
	 */
	T* construct()
	{
		void * mem = _pool.malloc();
		if (!mem)
			return nullptr;

		T* pobj = new(mem) T();
		return pobj;
	}

	/**
	 * @brief 销毁一个对象
	 * 
	 * @param pobj 要销毁的对象指针
	 * 
	 * 显式调用对象的析构函数，然后将内存返回给对象池以便重用
	 */
	void destroy(T* pobj)
	{
		pobj->~T();
		_pool.free(pobj);
	}

	/**
	 * @brief 手动释放未使用的内存
	 * 
	 * 将对象池中所有未使用的内存块释放回操作系统
	 * 这个操作可以减少内存占用，但可能会影响后续对象分配的性能
	 */
	void release()
	{
		_pool.release_memory();
	}
};

