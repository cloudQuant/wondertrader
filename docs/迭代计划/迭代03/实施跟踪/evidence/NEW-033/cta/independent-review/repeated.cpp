#include "WtCore/CtaStrategyMgr.h"
#include <cstdio>
int main(int argc, char** argv) {
 if (argc != 2) return 3;
 CtaStrategyMgr a; CtaStrategyMgr b;
 const bool first = a.loadFactories(argv[1]);
 const bool repeat = a.loadFactories(argv[1]);
 const bool independent = b.loadFactories(argv[1]);
 const auto strategy = b.createStrategy("stable", "unit", "b");
 std::printf("first=%d repeat=%d independent=%d strategy=%d\n", first, repeat, independent, strategy && strategy->self());
 return first && independent && strategy && strategy->self() ? 0 : 1;
}
