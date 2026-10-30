#pragma once

#include "IExecCommand.h"
#include <memory>
#include <vector>

NS_WTP_BEGIN

class WtExecuterFactory;

/**
 * Optional lifecycle interface for execution commands with asynchronous work.
 *
 * This is a sibling interface rather than a change to IExecCommand, so existing
 * executor implementations keep their current virtual table. requestQuiesce()
 * must be non-blocking and idempotent: it closes admission for new work while
 * retaining already accepted work. waitQuiesced() is called from a control
 * thread after all executors have received requestQuiesce(); it returns true
 * only when accepted work has drained. It must be able to finish without a
 * later executor's waitQuiesced() being called first; all coordination needed
 * between executors must be initiated by requestQuiesce() or ordinary work
 * completion. Implementations must return false when
 * called from one of their own worker/callback stacks rather than waiting for
 * themselves. waitQuiesced() may be called repeatedly after a false result; a
 * later executor may have failed its preflight or drain after this executor
 * already returned true, so implementations must tolerate repeated successful
 * waits without changing ownership or releasing the executor. Neither method
 * may release the executor or its external owners.
 */
class IExecCommandQuiesce
{
public:
	virtual ~IExecCommandQuiesce() {}
	virtual void requestQuiesce() = 0;
	/** Non-blocking preflight; false means waiting here could wait for this thread. */
	virtual bool canWaitQuiescedOnCurrentThread() const = 0;
	virtual bool waitQuiesced() = 0;
};

typedef std::shared_ptr<IExecCommandQuiesce> ExecQuiesceOwnerPtr;

/**
 * Optional staged contract for host execution commands that own plugin units.
 * This is a sibling interface: it does not change IExecCommand's virtual table.
 *
 * requestQuiesce() on the host command closes its ingress only. The manager
 * first closes and preflights every unique factory returned by
 * quiesceFactory(), then calls preflightQuiesceOnCurrentThread() for every
 * command. That host method must be non-blocking and check only whether the
 * current thread is on a host callback/ingress stack. Factory preflight is a
 * separate, deduplicated step; its first call closes factory load/config
 * admission and checks for in-flight loader work.
 *
 * drainHostQuiesced() waits only for host-owned ingress, callback drains, and
 * queues. It must not request or wait for plugin units, or depend on a later
 * host's drainHostQuiesced() call. Once every host has
 * drained, the coordinator closes unit admission and waits for all factory
 * create/init operations before taking stable host and factory unit snapshots.
 * snapshotQuiescePluginOwners() returns strong plugin-unit owners that may
 * still depend on this command's context. It must include active, failed,
 * retired, and otherwise retained units, remove duplicates, and return false
 * if a complete stable snapshot cannot be proved. The manager retains this
 * snapshot through all retries.
 *
 * After the coordinator has waited successfully for every plugin owner across
 * every manager, it calls completeQuiesceAfterGlobalPluginWait(). This final
 * acknowledgement must be non-blocking and idempotent. It may return false;
 * the coordinator then retains all owners and retries acknowledgements from a
 * control thread. An acknowledgement must never destroy or release command,
 * unit, context, factory, or module owners, even if this host acknowledged
 * before another host failed. Owners remain pinned until every host ack succeeds
 * and the coordinator publishes its shared global completion token. No executor
 * may treat a manager-local wait as this global completion signal.
 */
class IExecCommandQuiesceStages
{
public:
	virtual ~IExecCommandQuiesceStages() {}
	virtual bool preflightQuiesceOnCurrentThread() const = 0;
	virtual bool drainHostQuiesced() = 0;
	virtual bool snapshotQuiescePluginOwners(std::vector<ExecQuiesceOwnerPtr>& out) const = 0;
	virtual bool completeQuiesceAfterGlobalPluginWait() = 0;
	/** Borrowed factory identity; the host's upper owner must keep it alive. */
	virtual WtExecuterFactory* quiesceFactory() const = 0;
};

NS_WTP_END
