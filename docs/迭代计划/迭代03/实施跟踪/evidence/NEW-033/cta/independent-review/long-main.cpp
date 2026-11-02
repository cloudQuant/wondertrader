#include "/Users/yunjinqi/Documents/new_projects/wondertrader/src/WtCore/CtaStrategyMgr.h"
#include <string>
#include <cstdio>
int main(int argc, char** argv) { if (argc != 2) return 3; CtaStrategyMgr m; bool loaded=m.loadFactories(argv[1]); std::string name(4096,'x'); auto strategy=m.createStrategy(name.c_str(),"unit","z"); std::fprintf(stderr,"loaded=%d full_name_strategy=%d\n",loaded, bool(strategy && strategy->self())); return loaded && strategy && strategy->self() ? 0 : 1; }
