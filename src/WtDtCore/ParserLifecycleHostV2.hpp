/*!
 * \file ParserLifecycleHostV2.hpp
 * \brief Host-side helpers for the parser lifecycle V2 ABI used by the data
 *        kit parser adapters (WtDtCore and WtDtServo).
 *
 * Decision card 9 (9a=A+C, 9b=A, 9c=A, 9d=A): a dynamic parser provider must
 * export a valid V2 table and pass its factory preflight before createParser
 * is called. A provider API is destroyed and its module unloaded only after the
 * provider reports DRAINED; any other result keeps the API, module and borrowed
 * host objects (retryable PENDING, or permanent quarantine on uncertainty).
 *
 * The helpers only interpret the ABI; they do not log and never throw.
 */
#pragma once

#include "../Includes/IParserApi.h"
#include "../Includes/IParserLifecycleV2.h"
#include "../Share/DLLHelper.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace wt_dt_parser_v2
{
	enum class BindResult
	{
		Ok,
		MissingGetter,		// legacy module: rejected before createParser
		InvalidTable,		// malformed/unsupported table: rejected before createParser
		PreflightRejected,	// provider refused creation without side effects
		Uncertain			// table/preflight threw or returned ERROR: keep module mapped
	};

	enum class StepResult
	{
		Done,		// stop accepted / API destroyed
		Pending,	// retry later from a safe thread (PENDING or WOULD_DEADLOCK)
		Uncertain	// provider state cannot be established: quarantine permanently
	};

	inline BindResult bind_lifecycle(DllHandle module, const WtParserLifecycleApiV2*& table) noexcept
	{
		table = NULL;
		if (module == NULL)
			return BindResult::MissingGetter;

		WtParserLifecycleV2GetterFn getter = reinterpret_cast<WtParserLifecycleV2GetterFn>(
			DLLHelper::get_symbol(module, WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL));
		if (getter == NULL)
			return BindResult::MissingGetter;

		const WtParserLifecycleApiV2* candidate = NULL;
		try
		{
			candidate = getter(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR);
		}
		catch (...)
		{
			return BindResult::Uncertain;
		}

		if (candidate == NULL || candidate->struct_size < WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE ||
			(candidate->abi_version >> 16) != WT_PARSER_LIFECYCLE_V2_ABI_MAJOR ||
			(candidate->capabilities & WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES) != WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES ||
			candidate->reserved[0] != 0 || candidate->reserved[1] != 0 ||
			candidate->preflight_factory == NULL || candidate->request_stop == NULL || candidate->can_wait == NULL ||
			candidate->wait_drained == NULL || candidate->destroy_api == NULL)
			return BindResult::InvalidTable;

		WtParserLifecycleV2Status preflight = WT_PARSER_LIFECYCLE_V2_ERROR;
		try
		{
			preflight = candidate->preflight_factory();
		}
		catch (...)
		{
			return BindResult::Uncertain;
		}

		if (preflight != WT_PARSER_LIFECYCLE_V2_OK)
		{
			if (preflight == WT_PARSER_LIFECYCLE_V2_PENDING || preflight == WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK ||
				preflight == WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT || preflight == WT_PARSER_LIFECYCLE_V2_UNSUPPORTED)
				return BindResult::PreflightRejected;
			return BindResult::Uncertain;
		}

		table = candidate;
		return BindResult::Ok;
	}

	// Nonblocking and idempotent: once accepted the request is not repeated.
	inline StepResult request_stop(const WtParserLifecycleApiV2* table, wtp::IParserApi* api, bool& accepted) noexcept
	{
		if (accepted)
			return StepResult::Done;
		if (table == NULL || api == NULL)
			return StepResult::Uncertain;

		WtParserLifecycleV2Status status = WT_PARSER_LIFECYCLE_V2_ERROR;
		try
		{
			status = table->request_stop(static_cast<void*>(api));
		}
		catch (...)
		{
			return StepResult::Uncertain;
		}

		if (status == WT_PARSER_LIFECYCLE_V2_OK)
		{
			accepted = true;
			return StepResult::Done;
		}
		if (status == WT_PARSER_LIFECYCLE_V2_PENDING || status == WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK)
			return StepResult::Pending;
		return StepResult::Uncertain;
	}

	// Destroys the API only after an accepted stop, an OK can_wait probe on the
	// calling thread and a DRAINED result. api is cleared only on success.
	inline StepResult drain_and_destroy(const WtParserLifecycleApiV2* table, wtp::IParserApi*& api,
		bool stopAccepted, uint32_t timeoutMs) noexcept
	{
		if (api == NULL)
			return StepResult::Done;
		if (table == NULL)
			return StepResult::Uncertain;
		if (!stopAccepted)
			return StepResult::Pending;

		WtParserLifecycleV2Status status = WT_PARSER_LIFECYCLE_V2_ERROR;
		try
		{
			status = table->can_wait(static_cast<void*>(api));
		}
		catch (...)
		{
			return StepResult::Uncertain;
		}
		if (status != WT_PARSER_LIFECYCLE_V2_OK)
		{
			if (status == WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK || status == WT_PARSER_LIFECYCLE_V2_PENDING)
				return StepResult::Pending;
			return StepResult::Uncertain;
		}

		try
		{
			status = table->wait_drained(static_cast<void*>(api), timeoutMs);
		}
		catch (...)
		{
			return StepResult::Uncertain;
		}
		if (status != WT_PARSER_LIFECYCLE_V2_DRAINED)
		{
			if (status == WT_PARSER_LIFECYCLE_V2_PENDING || status == WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK)
				return StepResult::Pending;
			return StepResult::Uncertain;
		}

		void* opaque = static_cast<void*>(api);
		try
		{
			status = table->destroy_api(&opaque);
		}
		catch (...)
		{
			return StepResult::Uncertain;
		}
		if (status != WT_PARSER_LIFECYCLE_V2_OK || opaque != NULL)
			return StepResult::Uncertain;

		api = NULL;
		return StepResult::Done;
	}

	/*
	 * Host callback admission gate. Every IParserSpi entry enters the gate before
	 * touching borrowed host objects. close() stops admission; drained() reports
	 * whether no admitted callback is still running. Both use sequentially
	 * consistent atomics so a callback either observes the closed gate or is
	 * counted by the closer.
	 */
	class CallbackGate
	{
	private:
		struct Frame { const CallbackGate* gate; const Frame* prev; };
		static const Frame*& current_frame() noexcept
		{
			static thread_local const Frame* frame = NULL;
			return frame;
		}

	public:
		// Open on construction (matching the legacy adapters, whose _stopped
		// flag started false); init closes it until the provider is registered,
		// and every failure/stop path closes it again.
		CallbackGate() : _closed(false), _in_flight(0) {}

		void open() noexcept { _closed.store(false); }
		void close() noexcept { _closed.store(true); }
		bool closed() const noexcept { return _closed.load(); }
		bool drained() const noexcept { return _in_flight.load() == 0; }
		// Bounded wait for admitted host callbacks to leave (the gate must already
		// be closed). Never call it from inside one of this gate's callbacks.
		bool waitDrained(uint32_t timeoutMs) const noexcept
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
			while (!drained())
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}
		// True while the current thread is executing a callback admitted by this gate.
		bool inCallbackOnThisThread() const noexcept
		{
			for (const Frame* f = current_frame(); f != NULL; f = f->prev)
			{
				if (f->gate == this)
					return true;
			}
			return false;
		}

		class Scope
		{
		public:
			explicit Scope(CallbackGate& gate) noexcept : _gate(gate), _admitted(false), _frame{NULL, NULL}
			{
				_gate._in_flight.fetch_add(1);
				if (_gate._closed.load())
				{
					_gate._in_flight.fetch_sub(1);
					return;
				}
				_admitted = true;
				_frame.gate = &_gate;
				_frame.prev = current_frame();
				current_frame() = &_frame;
			}
			~Scope()
			{
				if (!_admitted)
					return;
				current_frame() = _frame.prev;
				_gate._in_flight.fetch_sub(1);
			}
			bool admitted() const noexcept { return _admitted; }

		private:
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;
			CallbackGate& _gate;
			bool _admitted;
			Frame _frame;
		};

	private:
		std::atomic<bool> _closed;
		std::atomic<uint32_t> _in_flight;
	};
}
