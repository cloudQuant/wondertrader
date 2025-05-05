/*!
 * @file SpinMutex.hpp
 * @brief 自旋互斥锁实现
 * 
 * 该文件实现了一个基于原子操作的自旋互斥锁(SpinMutex)和相应的RAII包装器(SpinLock)。
 * 自旋锁适用于预期锁定时间非常短的场景，可以避免线程睡眠和唤醒的开销。
 * 在多核处理器上，对于锁竞争不激烈且持锁时间极短的场景，自旋锁通常比普通互斥锁更高效。
 */

#pragma once
#include <atomic>
#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/**
 * @brief 自旋互斥锁类
 * 
 * 使用std::atomic<bool>实现的自旋互斥锁。与传统互斥锁不同，
 * 当自旋锁被占用时，尝试获取锁的线程会一直自旋(循环检查)直到获取锁成功，
 * 而不是进入睡眠状态。因此适合锁定时间极短的临界区。
 */
class SpinMutex
{
private:
	/**
	 * @brief 原子标志位
	 * 
	 * 标志位为true表示锁已被占用，false表示锁可用
	 * 使用std::atomic确保多线程访问的原子性
	 */
	std::atomic<bool> flag = { false };

public:
	/**
	 * @brief 获取锁
	 * 
	 * 尝试获取自旋锁。如果锁已被占用，则会循环等待直到获取成功。
	 * 等待过程中使用特定CPU指令提示处理器这是一个自旋等待，以优化性能。
	 * 使用std::memory_order_acquire保证获取锁后的内存操作不会被重排到获取锁之前。
	 */
	void lock()
	{
		for (;;)
		{
			// 尝试将flag从false原子地设置为true，如果成功(返回旧值false)则获取锁成功
			if (!flag.exchange(true, std::memory_order_acquire))
				break;
		
			// 如果获取失败，自旋等待直到锁可能被释放
			while (flag.load(std::memory_order_relaxed))
			{
//#ifdef _MSC_VER
//				_mm_pause();
//#else
#if defined(_MSC_VER)
                _mm_pause(); // Windows 平台使用_mm_pause()暂停指令
#elif defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause(); // x86平台使用PAUSE指令
#elif defined(__aarch64__) || defined(__arm__)
                asm volatile("yield" ::: "memory"); // ARM平台使用yield指令
#else
                // 其他平台，使用空操作作为CPU提示
                asm volatile("" ::: "memory");
#endif
			}
		}
	}

	/**
	 * @brief 释放锁
	 * 
	 * 释放自旋锁，将标志位设置回false。
	 * 使用std::memory_order_release保证释放锁前的内存操作不会被重排到释放锁之后。
	 */
	void unlock()
	{
		flag.store(false, std::memory_order_release);
	}
};

/**
 * @brief 自旋锁的RAII封装
 * 
 * 提供基于RAII(资源获取即初始化)原则的自旋锁包装器。
 * 构造时自动获取锁，析构时自动释放锁，避免忘记释放锁导致的死锁问题。
 * 禁止复制和赋值操作，确保锁语义的正确性。
 */
class SpinLock
{
public:
	/**
	 * @brief 构造函数
	 * 
	 * @param mtx 要锁定的SpinMutex引用
	 * 
	 * 在构造时自动获取锁。获取锁的操作可能会阻塞(自旋等待)。
	 */
	SpinLock(SpinMutex& mtx) :_mutex(mtx) { _mutex.lock(); }

	/**
	 * @brief 禁止复制构造
	 * 
	 * 自旋锁不应被复制，否则会导致锁语义错误
	 */
	SpinLock(const SpinLock&) = delete;

	/**
	 * @brief 禁止赋值操作
	 * 
	 * 自旋锁不应被赋值，否则会导致锁语义错误
	 * 
	 * @return SpinLock& 赋值运算符返回值
	 */
	SpinLock& operator=(const SpinLock&) = delete;

	/**
	 * @brief 析构函数
	 * 
	 * 在对象销毁时自动释放锁
	 */
	~SpinLock() { _mutex.unlock(); }

private:
	/**
	 * @brief 被包装的SpinMutex引用
	 * 
	 * 存储对被管理的自旋互斥锁的引用，用于在构造时锁定和析构时解锁
	 */
	SpinMutex&	_mutex;
};
