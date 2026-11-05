/*!
 * \file IndexWorkerMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include <atomic>
#include <memory>

#include "../Includes/FasterDefs.h"
#include "../Includes/IIndexDefs.h"
#include "../Share/DLLHelper.hpp"

#include <Share/threadpool.hpp>

USING_NS_WTP;

class DataManager;

class IdxWorkerWrapper
{
public:
	IdxWorkerWrapper(IIndexWorker* stra, IIndexWorkerFact* fact) :_worker(stra), _fact(fact){}
	~IdxWorkerWrapper()
	{
		if (_worker)
		{
			_fact->delete_worker(_worker);
		}
	}

	IIndexWorker* self(){ return _worker; }


private:
	IIndexWorker*		_worker;
	IIndexWorkerFact*	_fact;
};
typedef std::shared_ptr<IdxWorkerWrapper>	IndexWorkerPtr;


class IndexWorkerMgr : public IIndexContext
{
public:
	IndexWorkerMgr();
	~IndexWorkerMgr();

public:
	bool loadFactories(const char* path);

private:
	IndexWorkerPtr createWorker(const char* name, const char* id);

public:
	bool	init(WTSVariant* config, IHotMgr* hotMgr, IBaseDataMgr* bdMgr, DataManager* dataMgr);
	void	handle_quote(WTSTickData* newTick);

	// Teardown (R68-003). The caller must first stop every parser that can call
	// handle_quote (QuoteFactory stops and drains its parsers before this).
	// Stops admission, drains the worker pool, then deletes workers through their
	// own factory (virtual delete_worker, which joins worker threads). Factories
	// and their modules stay pinned to process exit: the exported
	// delete_index_factory takes the pointer by value while FuncDeleteIndexFact
	// declares it by reference, and no versioned export identifies which one a
	// module implements, so the host never calls a remover. Idempotent.
	void	release();
	bool	isReleased() const { return _released.load(); }

public:
	virtual IHotMgr*		get_hot_mgr() override { return _hot_mgr; }
	virtual IBaseDataMgr*	get_bd_mgr() override { return _bd_mgr; }

	virtual WTSTickData*	sub_ticks(const char* fullCode) override;
	virtual WTSTickData*	get_tick(const char* code, const char* exchg) override;
	virtual void			push_tick(WTSTickData* newTick) override;
	virtual void			output_log(WTSLogLevel ll, const char* message) override;

private:
	friend struct R18IndexWorkerMgrTestAccess;

	typedef struct _IdxFactInfo
	{
		std::string			_module_path;
		DllHandle			_module_inst;
		IIndexWorkerFact*	_fact;
		FuncCreateIndexFact	_creator;
		FuncDeleteIndexFact	_remover;
	} IdxFactInfo;
	typedef wt_hashmap<std::string, IdxFactInfo> StraFactMap;

	StraFactMap	_factories;

	// A loading or rejected factory may have started plugin work. Until callback
	// quiescence and the remover ABI are established, pin its object and module
	// instead of unloading them, and reject any further load attempt.
	bool _loading_factories = false;
	DllHandle _conflicting_module = nullptr;
	IIndexWorkerFact* _conflicting_factory = nullptr;

	typedef wt_hashmap<std::string, IndexWorkerPtr> IndexWorkerMap;
	IndexWorkerMap	_workers;

	IHotMgr*		_hot_mgr;
	IBaseDataMgr*	_bd_mgr;
	DataManager*	_data_mgr;

	typedef std::shared_ptr<boost::threadpool::pool> ThreadPoolPtr;
	ThreadPoolPtr	_pool;

	wt_hashset<std::string>	_subbed;
	std::atomic<bool>	_released{false};
};

