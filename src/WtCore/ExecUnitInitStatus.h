#pragma once

#include "../Includes/FasterDefs.h"
#include <stdint.h>
#include <string>

NS_WTP_BEGIN

/** Status of the latest initialization attempt for one execution unit. */
enum class ExecUnitInitState : uint8_t
{
	UNKNOWN = 0,
	INITIALIZING = 1,
	READY = 2,
	FAILED = 3
};

/** Result of looking up initialization status through the executor manager. */
enum class ExecUnitInitQueryResult : uint8_t
{
	OK = 0,
	EXECUTOR_NOT_FOUND = 1,
	UNSUPPORTED = 2,
	INVALID_ARGUMENT = 3
};

/** Result of explicitly clearing a retained failed initialization status. */
enum class ExecUnitInitClearResult : uint8_t
{
	CLEARED = 0,
	EXECUTOR_NOT_FOUND = 1,
	UNSUPPORTED = 2,
	INVALID_ARGUMENT = 3,
	NOT_FOUND = 4,
	NOT_FAILED = 5,
	RETRY_BLOCKED = 6
};

/** A copied status snapshot; error never points into executor-owned storage. */
struct ExecUnitInitStatus
{
	ExecUnitInitQueryResult query_result = ExecUnitInitQueryResult::OK;
	ExecUnitInitState state = ExecUnitInitState::UNKNOWN;
	uint64_t generation = 0;
	// Diagnostic count of calls currently waiting for this attempt to complete.
	uint32_t waiting_callers = 0;
	std::string error;
};

/**
 * Optional sibling interface for querying unit initialization without changing
 * IExecCommand's existing virtual table. Failed entries remain available until
 * explicitly cleared or replaced by a later initialization attempt. Callers
 * should clear terminal failures they no longer need: retained failures use
 * one record per failed standard code, with no fixed capacity limit.
 *
 * The runner snapshots the owning ExecCmdPtr under its lifecycle/registry locks
 * and invokes these methods after releasing those locks. A call can therefore
 * overlap runner shutdown. Implementations must be thread-safe and operate only
 * on provider-owned status state; they must not dereference runner-, engine-,
 * context-, trader-, or parser-owned objects. The ExecCmdPtr shared ownership
 * must also keep the implementation code module loaded until the call and the
 * executor object's destruction have both completed. A query that captured its
 * provider before shutdown may return its snapshot after shutdown has begun.
 */
class IExecUnitInitStatusProvider
{
public:
	virtual ~IExecUnitInitStatusProvider() {}
	virtual ExecUnitInitStatus queryUnitInitStatus(const char* stdCode) const = 0;
	virtual ExecUnitInitClearResult clearFailedUnitInitStatus(const char* stdCode) = 0;
};

NS_WTP_END
