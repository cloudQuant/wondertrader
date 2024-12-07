#pragma once
#include <atomic>
#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

//class SpinMutex
//{
//private:
//	std::atomic<bool> flag = { false };
//
//public:
//	void lock()
//	{
//		for (;;)
//		{
//			if (!flag.exchange(true, std::memory_order_acquire))
//				break;
//
//			while (flag.load(std::memory_order_relaxed))
//			{
//#ifdef _MSC_VER
//				_mm_pause();
//#else
//				__builtin_ia32_pause();
//#endif
//			}
//		}
//	}
//
//	void unlock()
//	{
//		flag.store(false, std::memory_order_release);
//	}
//};

// todo 1 2024-12-07 modify this class in order to run in ubuntu aarch64
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
#ifdef _MSC_VER
                _mm_pause();
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
                __builtin_ia32_pause();
#elif defined(__GNUC__) && (defined(__arm__) || defined(__aarch64__))
                __asm__ volatile("yield");
#else
                // Fallback for other architectures
                std::this_thread::yield();
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
