#pragma once
#include <atomic>
#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

class SpinMutex
{
private:
	std::atomic<bool> flag = { false };

public:
	void lock()
	{
		for (;;)
		{
			if (!flag.exchange(true, std::memory_order_acquire))
				break;

			while (flag.load(std::memory_order_relaxed))
			{
//#ifdef _MSC_VER
//				_mm_pause();
//#else
//				__builtin_ia32_pause();
//#endif
#if defined(_MSC_VER)
                _mm_pause(); // Windows 平台
#elif defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause(); // x86 平台
#elif defined(__aarch64__) || defined(__arm__)
                asm volatile("yield" ::: "memory"); // ARM 平台
#else
                // 其他平台，使用空操作
                asm volatile("" ::: "memory");
#endif
			}
		}
	}

	void unlock()
	{
		flag.store(false, std::memory_order_release);
	}
};

class SpinLock
{
public:
	SpinLock(SpinMutex& mtx) :_mutex(mtx) { _mutex.lock(); }
	SpinLock(const SpinLock&) = delete;
	SpinLock& operator=(const SpinLock&) = delete;
	~SpinLock() { _mutex.unlock(); }

private:
	SpinMutex&	_mutex;
};
