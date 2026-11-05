#pragma once
#include <cstdint>
#include <memory>
#include <strings.h>
#include <vector>
using CtxHandler=unsigned long;
inline int wt_stricmp(const char* a,const char* b){return strcasecmp(a,b);}
enum TaskPeriodType {TPT_None,TPT_Daily,TPT_Weekly,TPT_Monthly,TPT_Yearly,TPT_Minute};
struct Engine;
struct ExpSelContext {
 static unsigned made; unsigned _id;
 ExpSelContext(Engine*,const char*,int32_t):_id(++made){}
 unsigned id() const {return _id;}
};
using SelContextPtr=std::shared_ptr<ExpSelContext>;
struct Engine {
 std::vector<TaskPeriodType> types;
 std::vector<SelContextPtr> contexts;
 void addContext(SelContextPtr ctx,uint32_t,uint32_t,TaskPeriodType p,bool,const char*,const char*) {types.push_back(p); contexts.push_back(std::move(ctx));}
};
struct WtRtRunner {
 Engine _sel_engine;
 uint32_t createSelContext(const char*,uint32_t,uint32_t,const char*,int32_t,const char*,const char*);
};
WtRtRunner& getRunner();
CtxHandler create_sel_context(const char*,uint32_t,uint32_t,const char*,const char*,const char*,int32_t);
