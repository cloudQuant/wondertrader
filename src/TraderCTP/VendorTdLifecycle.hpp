/*!
 * \file VendorTdLifecycle.hpp
 * \project	WonderTrader
 *
 * \brief Legacy ITraderApi lifecycle helpers for traders whose callbacks come
 *        from a vendor SDK (TraderCTP, TraderCTPMini, TraderCTPOpt).
 *        Iteration 03, decision card 9 (9a=A+C, 9b=A, 9c=A, 9d=A);
 *        BUG-T25-005, T1-CTPOpt-01/02, T2-CTP-02/03, T3-006.
 *
 * Capability statement (what a provider using this header can and cannot prove):
 *  - Every host ITraderApi call that may touch the SDK API object holds an API
 *    lease, every SDK SPI callback holds a callback lease, and the provider's
 *    query worker marks its thread. Leases are lock-free admission counters,
 *    nest on one thread and never hold a lock across an SDK call, so a
 *    synchronous SDK callback raised from inside Req* (and a host call made from
 *    that callback) never deadlocks.
 *  - A terminal stop closes both gates first. A host thread that is not inside
 *    one of the provider's own frames then waits, with a bound, for the worker to
 *    exit and for every admitted lease to leave, and only then calls SDK
 *    Release() exactly once. SDK Release() therefore never overlaps an admitted
 *    Req* call or an admitted callback (T1-CTPOpt-02).
 *  - A stop requested on a thread that is inside one of the provider's own
 *    callbacks/API calls, or on its worker, only closes admission and records a
 *    deferred stop (9d). The next lifecycle call made from a safe thread
 *    (release/disconnect/deleteTrader/destructor) completes it.
 *  - When the bound expires (worker blocked inside an SDK call, a host callback
 *    blocked, ...) the SDK API is NOT released, no object is freed, and the
 *    instance reports DrainUncertain; a later release() may retry.
 *  - None of the vendored CTP/CTPMini/CTPOpt headers documents that Release(),
 *    RegisterSpi(NULL) or Logout() joins SDK threads or forbids a later callback.
 *    Once an SDK API object existed for an instance, the legacy deleteTrader
 *    remover therefore never deletes it: the instance is retained and the plugin
 *    module is pinned until process exit (9a quarantine, 9b). Callbacks reaching
 *    a retained instance after the stop are rejected by the closed gate.
 *  - Instances are single-use: connect() after a stop is rejected, because a late
 *    callback of the previous SDK API object cannot be told apart from one of a
 *    new API object (T2-CTP-03).
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wt_vendor_td
{
	static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
		"Vendor trader stop requires lock-free atomic operations");

	// Default bound of a legacy terminal stop (BUG-T25-005 acceptance: 2 seconds).
	static constexpr uint32_t DEFAULT_STOP_BOUND_MS = 2000;

	// Observable outcome of the void legacy release()/disconnect().
	enum class StopState : uint32_t
	{
		Running = 0,		// no stop requested yet
		StopDeferred = 1,	// stop requested from a provider frame; admission closed, SDK not released yet
		Released = 2,		// provider work drained, SDK Release() returned (or no SDK existed)
		DrainUncertain = 3	// bound expired; SDK not released, instance must stay alive (quarantine)
	};

	inline const char* stopStateName(StopState state) noexcept
	{
		switch (state)
		{
		case StopState::Running: return "running";
		case StopState::StopDeferred: return "stop-deferred";
		case StopState::Released: return "released";
		case StopState::DrainUncertain: return "drain-uncertain";
		}
		return "unknown";
	}

	// Admission gate: bit 63 closes admission permanently; the low bits count
	// admitted frames.
	class Gate
	{
	public:
		static constexpr uint64_t CLOSED = UINT64_C(1) << 63;
		static constexpr uint64_t COUNT_MASK = CLOSED - 1;

		bool enter() noexcept
		{
			uint64_t state = _state.load(std::memory_order_acquire);
			for (;;)
			{
				if ((state & CLOSED) != 0 || (state & COUNT_MASK) == COUNT_MASK)
					return false;
				if (_state.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire))
					return true;
			}
		}
		void leave() noexcept { _state.fetch_sub(1, std::memory_order_acq_rel); }
		void close() noexcept { _state.fetch_or(CLOSED, std::memory_order_acq_rel); }
		// Re-opens a closed gate only when no admitted frame remains. Providers never
		// call this (their stop is terminal); a host-side owner may re-arm explicitly.
		bool reopen() noexcept
		{
			uint64_t expected = CLOSED;
			return _state.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
		}
		bool closed() const noexcept { return (_state.load(std::memory_order_acquire) & CLOSED) != 0; }
		uint64_t active() const noexcept { return _state.load(std::memory_order_acquire) & COUNT_MASK; }

	private:
		std::atomic<uint64_t> _state{0};
	};

	// Per-thread stacks of active provider frames, keyed by provider object.
	struct Frame
	{
		const void* owner;
		Frame* previous;
	};
	inline Frame*& callbackFrames() noexcept { static thread_local Frame* top = nullptr; return top; }
	inline Frame*& apiFrames() noexcept { static thread_local Frame* top = nullptr; return top; }
	inline const void*& workerOwner() noexcept { static thread_local const void* owner = nullptr; return owner; }
	inline bool hasFrame(const Frame* top, const void* owner) noexcept
	{
		for (const Frame* frame = top; frame != nullptr; frame = frame->previous)
		{
			if (frame->owner == owner)
				return true;
		}
		return false;
	}

	// RAII admission through one gate, recorded on the current thread's stack.
	class Scope
	{
	public:
		Scope(Gate& gate, Frame*& top, const void* owner) noexcept
			: _gate(gate), _top(top), _frame{owner, top}, _entered(gate.enter())
		{
			if (_entered)
				_top = &_frame;
		}
		~Scope()
		{
			if (_entered)
			{
				_top = _frame.previous;
				_gate.leave();
			}
		}
		explicit operator bool() const noexcept { return _entered; }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		Gate&	_gate;
		Frame*&	_top;
		Frame	_frame;
		bool	_entered;
	};

	// Marks the current thread as the provider's query worker for its lifetime.
	class WorkerMark
	{
	public:
		explicit WorkerMark(const void* owner) noexcept : _previous(workerOwner()) { workerOwner() = owner; }
		~WorkerMark() { workerOwner() = _previous; }
		WorkerMark(const WorkerMark&) = delete;
		WorkerMark& operator=(const WorkerMark&) = delete;

	private:
		const void* _previous;
	};

	class Lifecycle
	{
	public:
		Gate api;			// host ITraderApi calls and worker requests that may touch the SDK
		Gate callbacks;		// SDK SPI callbacks
		std::atomic<bool> sdk_created{false};		// an SDK API object existed for this instance
		std::atomic<bool> stop_requested{false};
		std::atomic<bool> worker_running{false};	// set before a provider worker starts, cleared on exit
		std::atomic<uint32_t> state{static_cast<uint32_t>(StopState::Running)};
		std::mutex stop_mutex;	// serializes the completing part of a stop on safe threads

		Scope call(const void* owner) noexcept { return Scope(api, apiFrames(), owner); }
		Scope callback(const void* owner) noexcept { return Scope(callbacks, callbackFrames(), owner); }

		// True when the current thread is inside one of owner's callbacks or API
		// calls, or is owner's worker: waiting there for a drain could self-deadlock.
		static bool reentered(const void* owner) noexcept
		{
			return hasFrame(callbackFrames(), owner) || hasFrame(apiFrames(), owner) || workerOwner() == owner;
		}

		bool quiet() const noexcept { return api.active() == 0 && callbacks.active() == 0; }
		StopState stopState() const noexcept { return static_cast<StopState>(state.load(std::memory_order_acquire)); }
		void setState(StopState next) noexcept { state.store(static_cast<uint32_t>(next), std::memory_order_release); }

		// Closes host-call and callback admission for good.
		void close() noexcept
		{
			stop_requested.store(true, std::memory_order_release);
			api.close();
			callbacks.close();
		}

		// Polls pred until it holds or the deadline passes.
		template <typename Pred>
		static bool waitUntil(std::chrono::steady_clock::time_point deadline, Pred&& pred)
		{
			while (!pred())
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return pred();
				std::this_thread::sleep_for(std::chrono::microseconds(500));
			}
			return true;
		}
	};

	// Pins the module that contains this translation unit, so a host that unloads
	// it after the remover cannot unmap code a retained instance may still run
	// (9a). The pin is intentionally never undone.
	namespace detail
	{
		static const char module_anchor = 0;
	}
	inline bool pinCurrentModule() noexcept
	{
#ifdef _WIN32
		HMODULE handle = NULL;
		return ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCSTR>(&detail::module_anchor), &handle) != FALSE;
#else
		Dl_info info;
		if (::dladdr(static_cast<const void*>(&detail::module_anchor), &info) == 0 || info.dli_fname == nullptr)
			return false;
		// RTLD_NOLOAD only adds a reference to the already loaded image and
		// RTLD_NODELETE keeps it mapped even after every dlclose.
		return ::dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != nullptr;
#endif
	}

	// Keeps a provider instance reachable until process exit (9a/9b quarantine).
	// The registry itself is never destroyed.
	inline size_t quarantine(void* instance) noexcept
	{
		static std::mutex* mtx = new std::mutex();
		static std::vector<void*>* retained = new std::vector<void*>();
		try
		{
			pinCurrentModule();
			std::lock_guard<std::mutex> lock(*mtx);
			retained->push_back(instance);
			return retained->size();
		}
		catch (...)
		{
			// Allocation failed: the instance is still never freed by the caller.
			return 0;
		}
	}
}
