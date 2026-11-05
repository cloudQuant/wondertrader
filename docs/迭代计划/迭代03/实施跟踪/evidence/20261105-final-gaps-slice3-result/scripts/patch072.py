import sys
root=sys.argv[1]
def patch(path, pairs, crlf):
    p=root+'/'+path
    b=open(p,'rb').read()
    for old,new in pairs:
        if crlf:
            old=old.replace(b'\n',b'\r\n'); new=new.replace(b'\n',b'\r\n')
        n=b.count(old)
        assert n==1,(path,old[:80],n)
        b=b.replace(old,new)
    open(p,'wb').write(b)
    print('patched',path)

patch('src/WtCore/WtDiffExecuter.h',[
(b"""		uint32_t waiting_callers = 0;
		std::string init_error;
	};
""",b"""		uint32_t waiting_callers = 0;
		std::string init_error;
		// I03-NEW-072 (card 9d=A): adapter callbacks that arrived while this
		// generation was being created and before its unit was registered. They
		// are queued behind the unit's own registration callbacks in arrival
		// order, or dropped with a warning when the creation fails.
		std::deque<std::function<void(const ExecuteUnitPtr&)>> deferred_events;
	};
"""),
(b"""	bool enqueueUnitCallback(const std::shared_ptr<UnitLifecycleState>& state,
		std::function<void()> callback);
""",b"""	bool enqueueUnitCallback(const std::shared_ptr<UnitLifecycleState>& state,
		std::function<void()> callback);
	bool deferPendingUnitCallback(const std::string& code,
		std::function<void(const ExecuteUnitPtr&)> callback);
"""),
], True)

patch('src/WtCore/WtDiffExecuter.cpp',[
# helper after scheduleUnitCallbackDrain declaration site: insert before scheduleUnitCallbackDrain definition
(b"""void WtDiffExecuter::scheduleUnitCallbackDrain(const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state, bool schedule)
{""",b"""bool WtDiffExecuter::deferPendingUnitCallback(const std::string& code,
	std::function<void(const ExecuteUnitPtr&)> callback)
{
	// I03-NEW-072 (card 9d=A): an adapter callback never waits for a creation in
	// flight (on another thread, or re-entrantly on this one). The event joins
	// that creation instead: before registration it is kept with the generation
	// and queued behind the registration callbacks; after registration it joins
	// the FIFO that the creator drains before publishing READY. Holding the unit
	// registry and state locks keeps both the completion and the failure path
	// from running in between. Returns false when no creation is pending, so the
	// caller resolves the unit again.
	if (!callback)
		return false;
	SpinLock lock(_mtx_units);
	auto stateIt = _unit_states.find(code);
	if (stateIt == _unit_states.end())
		return false;
	const std::shared_ptr<UnitLifecycleState> state = stateIt->second;
	std::lock_guard<std::mutex> stateLock(state->mutex);
	if (state->creation_complete)
		return false;
	if (state->callback_failed)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return true;
	}
	auto unitIt = _unit_map.find(code);
	if (unitIt == _unit_map.end())
	{
		state->deferred_events.push_back(std::move(callback));
		return true;
	}
	ExecuteUnitPtr registered = unitIt->second;
	state->callback_events.push_back([registered, callback]() { callback(registered); });
	return true;
}

void WtDiffExecuter::scheduleUnitCallbackDrain(const ExecuteUnitPtr& unit,
	const std::shared_ptr<UnitLifecycleState>& state, bool schedule)
{"""),
# registerCreatedUnit: append deferred events after registration callbacks
(b"""	else if (pendingTarget && hasRecoveryDiff)
	{
		enqueueUnitCallback(state, [this, code, unit, recoveryDiff]() {
			if (!_trader || _trader->checkOrderLimits(code.c_str()))
				unit->self()->set_position(code.c_str(), recoveryDiff);
		});
	}
	return true;
}""",b"""	else if (pendingTarget && hasRecoveryDiff)
	{
		enqueueUnitCallback(state, [this, code, unit, recoveryDiff]() {
			if (!_trader || _trader->checkOrderLimits(code.c_str()))
				unit->self()->set_position(code.c_str(), recoveryDiff);
		});
	}
	// I03-NEW-072: adapter callbacks that arrived during the creation follow the
	// unit's own channel/recovery callbacks, in arrival order.
	std::deque<std::function<void(const ExecuteUnitPtr&)>> deferred;
	{
		std::lock_guard<std::mutex> stateLock(state->mutex);
		deferred.swap(state->deferred_events);
	}
	for (auto& event : deferred)
	{
		std::function<void(const ExecuteUnitPtr&)> deliver = std::move(event);
		enqueueUnitCallback(state, [unit, deliver]() { deliver(unit); });
	}
	return true;
}"""),
# failCreation: count and drop queued callbacks
(b"""		std::string failureReason = reason ? reason : "unknown execution-unit initialization failure";
		bool persistFailure = false;""",b"""		std::string failureReason = reason ? reason : "unknown execution-unit initialization failure";
		bool persistFailure = false;
		size_t droppedCallbacks = 0;"""),
(b"""			state->creation_owner = std::thread::id();
			state->callback_events.clear();
		}
		state->condition.notify_all();""",b"""			state->creation_owner = std::thread::id();
			droppedCallbacks = state->deferred_events.size() + state->callback_events.size();
			state->deferred_events.clear();
			state->callback_events.clear();
		}
		state->condition.notify_all();
		if (droppedCallbacks != 0)
			_quiesce_incomplete.store(true, std::memory_order_release);"""),
(b"""			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"Diff execution unit initialization for {} failed: {}", stdCode, failureReason);
		}""",b"""			WTSLogger::log_dyn("executer", _name.c_str(), LL_ERROR,
				"Diff execution unit initialization for {} failed: {}", stdCode, failureReason);
			if (droppedCallbacks != 0)
				WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN,
					"[{}] {} deferred callback(s) for {} were dropped because its execution unit failed to initialize",
					_name, droppedCallbacks, stdCode);
		}"""),
# on_tick
(b"""	if (!ownsCode(code))
		return;
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return;
	}
	if (_unit_lookup_test_hook && stdCode != NULL)
		_unit_lookup_test_hook(stdCode);""",b"""	if (!ownsCode(code))
		return;
	bool creationPending = false;
	ExecuteUnitPtr unit = getUnit(stdCode, false, false, &creationPending);
	if (unit == NULL && creationPending)
	{
		// I03-NEW-072: do not block the adapter callback thread on the creation.
		newTick->retain();
		std::shared_ptr<WTSTickData> pendingTick(newTick, [](WTSTickData* value) { value->release(); });
		if (deferPendingUnitCallback(code, [pendingTick](const ExecuteUnitPtr& target) {
			target->self()->on_tick(pendingTick.get());
		}))
			return;
		unit = getUnit(stdCode, false, false, &creationPending);
	}
	if (unit == NULL)
	{
		_quiesce_incomplete.store(true, std::memory_order_release);
		return;
	}
	if (_unit_lookup_test_hook && stdCode != NULL)
		_unit_lookup_test_hook(stdCode);"""),
# on_trade
(b"""	if (!ownsCode(code))
		return;
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (_unit_lookup_test_hook)
		_unit_lookup_test_hook(code);
	std::shared_ptr<UnitLifecycleState> state = unit ? getUnitState(stdCode, unit) :
		std::shared_ptr<UnitLifecycleState>();
	const bool pluginAvailable = unit != NULL && state != NULL;
""",b"""	if (!ownsCode(code))
		return;
	bool creationPending = false;
	ExecuteUnitPtr unit = getUnit(stdCode, false, false, &creationPending);
	if (_unit_lookup_test_hook)
		_unit_lookup_test_hook(code);
	std::shared_ptr<UnitLifecycleState> state = unit ? getUnitState(stdCode, unit) :
		std::shared_ptr<UnitLifecycleState>();
	bool pluginAvailable = unit != NULL && state != NULL;
	bool deferred = false;
"""),
(b"""		else if (pluginAvailable)
			scheduleDrain = enqueueUnitCallback(state, [localid, unit, code, isBuy, vol, price]() {
				unit->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
			});
	}

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff of {} updated by trade: {} -> {}", _name, stdCode, prevDiff, curDiff);
	if (!pluginAvailable)""",b"""		else if (pluginAvailable)
			scheduleDrain = enqueueUnitCallback(state, [localid, unit, code, isBuy, vol, price]() {
				unit->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
			});
		else if (creationPending)
		{
			// I03-NEW-072: the diff is booked now; the plugin notification joins the
			// creation in flight instead of blocking this callback thread.
			deferred = deferPendingUnitCallback(code, [localid, code, isBuy, vol, price](const ExecuteUnitPtr& target) {
				target->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
			});
			if (!deferred)
			{
				unit = getUnit(stdCode, false, false, &creationPending);
				state = unit ? getUnitState(stdCode, unit) : std::shared_ptr<UnitLifecycleState>();
				pluginAvailable = unit != NULL && state != NULL;
				if (pluginAvailable)
					scheduleDrain = enqueueUnitCallback(state, [localid, unit, code, isBuy, vol, price]() {
						unit->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
					});
			}
		}
	}

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff of {} updated by trade: {} -> {}", _name, stdCode, prevDiff, curDiff);
	if (!pluginAvailable && !deferred)"""),
# on_order
(b"""	if (!ownsCode(code))
		return;
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
	{
		// Order updates are durable lifecycle information, unlike ticks. Losing
		// one makes a successful quiesce claim unsafe.""",b"""	if (!ownsCode(code))
		return;
	bool creationPending = false;
	ExecuteUnitPtr unit = getUnit(stdCode, false, false, &creationPending);
	if (unit == NULL && creationPending)
	{
		// I03-NEW-072: do not block the adapter callback thread on the creation.
		if (deferPendingUnitCallback(code, [localid, code, isBuy, leftQty, price, isCanceled](const ExecuteUnitPtr& target) {
			target->self()->on_order(localid, code.c_str(), isBuy, leftQty, price, isCanceled);
		}))
			return;
		unit = getUnit(stdCode, false, false, &creationPending);
	}
	if (unit == NULL)
	{
		// Order updates are durable lifecycle information, unlike ticks. Losing
		// one makes a successful quiesce claim unsafe."""),
# on_entrust
(b"""	if (!ownsCode(code))
		return;
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
	{
		// Entrust results must not disappear behind a successful drain result.""",b"""	if (!ownsCode(code))
		return;
	const bool hasMessage = message != NULL;
	const std::string msg = hasMessage ? message : "";
	bool creationPending = false;
	ExecuteUnitPtr unit = getUnit(stdCode, false, false, &creationPending);
	if (unit == NULL && creationPending)
	{
		// I03-NEW-072: do not block the adapter callback thread on the creation.
		if (deferPendingUnitCallback(code, [localid, code, bSuccess, msg, hasMessage](const ExecuteUnitPtr& target) {
			target->self()->on_entrust(localid, code.c_str(), bSuccess, hasMessage ? msg.c_str() : NULL);
		}))
			return;
		unit = getUnit(stdCode, false, false, &creationPending);
	}
	if (unit == NULL)
	{
		// Entrust results must not disappear behind a successful drain result."""),
(b"""	const bool hasMessage = message != NULL;
	const std::string msg = hasMessage ? message : "";
	const bool scheduleDrain = enqueueUnitCallback(state, [unit, localid, code, bSuccess, msg, hasMessage]() {""",b"""	const bool scheduleDrain = enqueueUnitCallback(state, [unit, localid, code, bSuccess, msg, hasMessage]() {"""),
], False)
