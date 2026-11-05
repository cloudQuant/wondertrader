#include "TraderDumper.h"
#include "Dumper.h"

#include "../Share/ModuleHelper.hpp"

Dumper& getDumper()
{
	// T3-012 / 卡9a：导出单例保活到进程退出，不在静态析构期销毁仍可能被
	// worker、交易通道回调或用户回调使用的对象。
	static Dumper* dumper = new Dumper();
	return *dumper;
}

void register_callbacks(FuncOnAccount cbAccount, FuncOnOrder  cbOrder, FuncOnTrade cbTrade, FuncOnPosition cbPosition)
{
	getDumper().register_callbacks(cbAccount, cbOrder, cbTrade, cbPosition);
}

void init(const char* logProfile)
{
	getDumper().init(logProfile);
}

bool config(const char* cfgfile, bool isFile)
{
	return getDumper().config(cfgfile, isFile, getBinDir());
}

void run(bool bOnce)
{
	getDumper().run(bOnce);
}

void release()
{
	getDumper().release();
}