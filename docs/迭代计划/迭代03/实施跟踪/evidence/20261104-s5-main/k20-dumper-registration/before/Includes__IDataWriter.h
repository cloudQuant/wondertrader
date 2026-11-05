/*!
 * \file IDataWriter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据落地接口定义
 */
#pragma once
#include <stdint.h>
#include "WTSTypes.h"
#include "FasterDefs.h"

NS_WTP_BEGIN
class WTSTickData;
class WTSOrdQueData;
class WTSOrdDtlData;
class WTSTransData;
class WTSVariant;
class IBaseDataMgr;
struct WTSBarStruct;
struct WTSTickStruct;
struct WTSOrdDtlStruct;
struct WTSOrdQueStruct;
struct WTSTransStruct;

class IDataWriterSink
{
public:
	
	virtual IBaseDataMgr* getBDMgr() = 0;

	virtual bool canSessionReceive(const char* sid) = 0;

	virtual void broadcastTick(WTSTickData* curTick) = 0;

	virtual void broadcastOrdQue(WTSOrdQueData* curOrdQue) = 0;

	virtual void broadcastOrdDtl(WTSOrdDtlData* curOrdDtl) = 0;

	virtual void broadcastTrans(WTSTransData* curTrans) = 0;

	virtual CodeSet* getSessionComms(const char* sid) = 0;

	virtual uint32_t getTradingDate(const char* pid) = 0;

	/*
	*	处理解析模块的日志
	*	@ll			日志级别
	*	@message	日志内容
	*/
	virtual void outputLog(WTSLogLevel ll, const char* message) = 0;
};

class IHisDataDumper
{
public:
	virtual bool dumpHisBars(const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count) = 0;
	virtual bool dumpHisTicks(const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count) = 0;

	virtual bool dumpHisOrdQue(const char* /*stdCode*/, uint32_t /*uDate*/, WTSOrdQueStruct* /*items*/, uint32_t /*count*/) { return false; }
	virtual bool dumpHisOrdDtl(const char* /*stdCode*/, uint32_t /*uDate*/, WTSOrdDtlStruct* /*items*/, uint32_t /*count*/) { return false; }
	virtual bool dumpHisTrans(const char* /*stdCode*/, uint32_t /*uDate*/, WTSTransStruct* /*items*/, uint32_t /*count*/) { return false; }
};

typedef wt_hashmap<std::string, IHisDataDumper*> ExtDumpers;

/*
 *	数据落地接口
 */
class IDataWriter
{
public:
	IDataWriter():_sink(NULL){}
	virtual ~IDataWriter() = default;

	virtual bool init(WTSVariant* /*params*/, IDataWriterSink* sink) { _sink = sink; return true; }

	virtual void release() = 0;

	void	add_ext_dumper(const char* id, IHisDataDumper* dumper) { _dumpers[id] = dumper; }

public:
	virtual bool writeTick(WTSTickData* curTick, uint32_t procFlag) = 0;

	virtual bool writeOrderQueue(WTSOrdQueData* /*curOrdQue*/) { return false; }

	virtual bool writeOrderDetail(WTSOrdDtlData* /*curOrdDetail*/) { return false; }

	virtual bool writeTransaction(WTSTransData* /*curTrans*/) { return false; }

	virtual void transHisData(const char* /*sid*/) {}

	virtual bool isSessionProceeded(const char* /*sid*/) { return true; }

	virtual WTSTickData* getCurTick(const char* code, const char* exchg = "") = 0;

protected:
	ExtDumpers			_dumpers;
	IDataWriterSink*	_sink;
};

NS_WTP_END


//获取IDataWriter的函数指针类型
typedef wtp::IDataWriter* (*FuncCreateWriter)();
typedef void(*FuncDeleteWriter)(wtp::IDataWriter* &writer);

/*
 * Versioned writer lifecycle C ABI V1 (iteration 03, decision card 9: 9a=A+C,
 * 9b=A, 9d=A). This block is additive: the IDataWriter/IDataWriterSink classes,
 * their vtables, member layout and the createWriter/deleteWriter exports above
 * are unchanged, so writers built against the old header still load.
 *
 * A writer module may export WT_DATA_WRITER_LIFECYCLE_V1_GETTER_SYMBOL. The getter
 * returns a provider-owned static table (NULL for an unknown major). The opaque
 * `writer` argument is exactly the IDataWriter* returned by that module's
 * createWriter, converted to void*; providers must reject pointers they did not
 * create. No callback may let a C++ exception cross this boundary.
 *
 *  - request_stop: nonblocking and idempotent. Callable from any thread, including
 *    from inside IDataWriterSink/IHisDataDumper callbacks and from writer-owned
 *    threads. Closes admission (later public calls fail) while accepted work keeps
 *    draining. Valid for an uninitialized writer and after a failed init.
 *  - can_wait: nonblocking probe for the calling thread. OK only when this thread
 *    may block in wait_drained without waiting for itself (it is not a writer-owned
 *    thread and is not inside an operation/callback of this writer); otherwise
 *    WOULD_DEADLOCK. The host must not call wait_drained without a fresh OK.
 *  - wait_drained: called after request_stop and a successful can_wait. timeout_ms
 *    0 polls; 0xFFFFFFFF waits without limit. DRAINED means: no writer operation is
 *    in flight, every accepted item has been processed, every writer-owned thread
 *    has exited and been joined, and no IDataWriterSink/IHisDataDumper callback is
 *    executing or can begin later. After DRAINED, IDataWriter::release() is a
 *    synchronous, callback-free cleanup. PENDING may be retried; WOULD_DEADLOCK and
 *    ERROR leave the writer live.
 *
 * Host contract: only DRAINED authorizes deleteWriter and unloading the module.
 * A writer whose module does not export this getter cannot prove callback
 * quiescence: the host calls release() from a safe (non-callback) context and then
 * retains the writer, its module and its sink for the life of the process
 * (quarantine). A getter that is present but invalid is rejected before
 * createWriter is called.
 */
#define WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR 1u
#define WT_DATA_WRITER_LIFECYCLE_V1_ABI_MINOR 0u
#define WT_DATA_WRITER_LIFECYCLE_V1_ABI_VERSION \
	((WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR << 16) | WT_DATA_WRITER_LIFECYCLE_V1_ABI_MINOR)
#define WT_DATA_WRITER_LIFECYCLE_V1_GETTER_SYMBOL "getDataWriterLifecycleApiV1"
#define WT_DATA_WRITER_LIFECYCLE_V1_WAIT_FOREVER 0xFFFFFFFFu

typedef int32_t WtDataWriterLifecycleV1Status;
enum
{
	WT_DATA_WRITER_LIFECYCLE_V1_OK = 0,
	WT_DATA_WRITER_LIFECYCLE_V1_DRAINED = 1,
	WT_DATA_WRITER_LIFECYCLE_V1_PENDING = 2,
	WT_DATA_WRITER_LIFECYCLE_V1_WOULD_DEADLOCK = 3,
	WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT = -1,
	WT_DATA_WRITER_LIFECYCLE_V1_ERROR = -3
};

/* Required capability: the three callbacks below implement the drain contract. */
#define WT_DATA_WRITER_LIFECYCLE_V1_CAP_DRAIN (UINT64_C(1) << 0)

typedef WtDataWriterLifecycleV1Status (*WtDataWriterLifecycleV1RequestStopFn)(void* writer);
typedef WtDataWriterLifecycleV1Status (*WtDataWriterLifecycleV1CanWaitFn)(void* writer);
typedef WtDataWriterLifecycleV1Status (*WtDataWriterLifecycleV1WaitDrainedFn)(void* writer, uint32_t timeout_ms);

/* Minor versions of major 1 are append-only; hosts accept a larger struct_size. */
typedef struct WtDataWriterLifecycleApiV1
{
	uint32_t struct_size;
	uint32_t abi_version;
	uint64_t capabilities;
	WtDataWriterLifecycleV1RequestStopFn request_stop;
	WtDataWriterLifecycleV1CanWaitFn can_wait;
	WtDataWriterLifecycleV1WaitDrainedFn wait_drained;
} WtDataWriterLifecycleApiV1;

typedef const WtDataWriterLifecycleApiV1* (*WtDataWriterLifecycleV1GetterFn)(uint32_t requested_major);