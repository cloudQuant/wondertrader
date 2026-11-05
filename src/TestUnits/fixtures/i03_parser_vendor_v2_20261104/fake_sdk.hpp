// Shared local fake-vendor-SDK machinery for the vendor parser V2 fixture.
// Each fake SDK library owns one callback thread per API object (the SDK
// thread); test code drives callbacks onto it through a small C control API.
// Nothing here opens sockets or contacts a vendor.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#define FAKE_MD_EXPORT extern "C" __attribute__((visibility("default")))

namespace fakesdk
{
	enum Counter
	{
		C_CREATED,					// API objects returned by the creator
		C_INIT,						// Init()/Login() starting the SDK thread
		C_RELEASE,					// Release() calls
		C_RELEASE_ON_SDK_THREAD,	// Release() called from the SDK callback thread (real SDKs self-join)
		C_RELEASE_DURING_CALLBACK,	// Release() while the SDK thread is inside a callback
		C_SPI_NULL,					// RegisterSpi(NULL)
		C_DISPATCHED,				// callbacks delivered to the registered SPI
		C_LATE_DISPATCHED,			// callbacks delivered through a stale SPI pointer during Release()
		C_LOGIN_REQ,
		C_SUB_REQ,
		C_LOGOUT,
		C_COUNT
	};

	enum Option { OPT_CREATE_NULL = 1, OPT_LATE_CALLBACK = 2, OPT_BLOCK_SUBSCRIBE = 3, OPT_LOGIN_RESULT = 4 };
	enum Kind { K_FRONT_CONNECTED = 1, K_LOGIN = 2, K_DEPTH = 3, K_DISCONNECTED = 4 };

	inline std::atomic<int>& counter(int index)
	{
		static std::atomic<int> counters[C_COUNT];
		return counters[index];
	}
	inline std::atomic<int>& option(int index)
	{
		static std::atomic<int> options[8];
		return options[index & 7];
	}

	class SdkThread
	{
	public:
		void start()
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (_thread.joinable())
				return;
			_stop = false;
			_thread = std::thread([this]() { run(); });
		}
		bool running()
		{
			std::lock_guard<std::mutex> lock(_mutex);
			return _thread.joinable() && !_stop;
		}
		bool post(std::function<void()> task)
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (!_thread.joinable() || _stop)
				return false;
			_queue.push_back(std::move(task));
			_cv.notify_all();
			return true;
		}
		bool waitIdle(int ms)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			return _cv.wait_for(lock, std::chrono::milliseconds(ms), [this]() { return _queue.empty() && !_busy; });
		}
		bool busy()
		{
			std::lock_guard<std::mutex> lock(_mutex);
			return _busy;
		}
		bool onThread() const { return std::this_thread::get_id() == _id.load(); }
		// Stops and joins the SDK thread; refuses (returns false) a self-join.
		bool stopJoin()
		{
			if (onThread())
				return false;
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_stop = true;
				_cv.notify_all();
			}
			if (_thread.joinable())
				_thread.join();
			return true;
		}
		~SdkThread()
		{
			if (!onThread())
				stopJoin();
			else if (_thread.joinable())
				_thread.detach();
		}

	private:
		void run()
		{
			_id.store(std::this_thread::get_id());
			for (;;)
			{
				std::function<void()> task;
				{
					std::unique_lock<std::mutex> lock(_mutex);
					_cv.wait(lock, [this]() { return _stop || !_queue.empty(); });
					if (_stop)
					{
						_queue.clear();
						_cv.notify_all();
						break;
					}
					task = std::move(_queue.front());
					_queue.pop_front();
					_busy = true;
				}
				task();
				{
					std::lock_guard<std::mutex> lock(_mutex);
					_busy = false;
					_cv.notify_all();
				}
			}
		}

		std::mutex _mutex;
		std::condition_variable _cv;
		std::deque<std::function<void()>> _queue;
		std::thread _thread;
		std::atomic<std::thread::id> _id{};
		bool _stop = false;
		bool _busy = false;
	};

	// One-shot gate used to hold a vendor request call (OPT_BLOCK_SUBSCRIBE).
	class Latch
	{
	public:
		void wait(int ms)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_entered = true;
			_cv.notify_all();
			_cv.wait_for(lock, std::chrono::milliseconds(ms), [this]() { return _open; });
		}
		bool waitEntered(int ms)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			return _cv.wait_for(lock, std::chrono::milliseconds(ms), [this]() { return _entered; });
		}
		void open()
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_open = true;
			_cv.notify_all();
		}
		void reset()
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_open = false;
			_entered = false;
		}

	private:
		std::mutex _mutex;
		std::condition_variable _cv;
		bool _open = false;
		bool _entered = false;
	};
	inline Latch& subscribeLatch() { static Latch latch; return latch; }

	// Test-provided probe: returns non-zero while the host sink is still inside
	// a callback. Release() uses it to detect a vendor release that overlaps
	// host callback execution (falls back to the SDK thread's busy flag).
	typedef int (*ReleaseProbe)();
	inline std::atomic<ReleaseProbe>& releaseProbe() { static std::atomic<ReleaseProbe> probe{nullptr}; return probe; }
	inline bool releaseOverlapsCallback(bool sdkBusy)
	{
		ReleaseProbe probe = releaseProbe().load();
		return probe ? probe() != 0 : sdkBusy;
	}

	inline void resetAll()
	{
		for (int i = 0; i < C_COUNT; ++i)
			counter(i).store(0);
		for (int i = 0; i < 8; ++i)
			option(i).store(0);
		subscribeLatch().reset();
		releaseProbe().store(nullptr);
	}
}

// Control API shared by every fake SDK library (each test process loads one).
// fake_md_post/fake_md_wait_idle/fake_md_on_sdk_thread are defined per family.
#define FAKE_MD_COMMON_CONTROL() \
	FAKE_MD_EXPORT int fake_md_counter(int index) { return (index >= 0 && index < fakesdk::C_COUNT) ? fakesdk::counter(index).load() : -1; } \
	FAKE_MD_EXPORT void fake_md_reset() { fakesdk::resetAll(); } \
	FAKE_MD_EXPORT void fake_md_set(int index, int value) { fakesdk::option(index).store(value); } \
	FAKE_MD_EXPORT int fake_md_wait_subscribe_entered(int ms) { return fakesdk::subscribeLatch().waitEntered(ms) ? 1 : 0; } \
	FAKE_MD_EXPORT void fake_md_unblock_subscribe() { fakesdk::subscribeLatch().open(); } \
	FAKE_MD_EXPORT void fake_md_set_release_probe(fakesdk::ReleaseProbe probe) { fakesdk::releaseProbe().store(probe); }
