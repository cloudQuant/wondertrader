#include "WtCore/CtaStrategyMgr.h"
#include <cstdio>
int main(int argc, char** argv) {
 if (argc != 3) return 3;
 CtaStrategyMgr a; CtaStrategyMgr b;
 const bool first = a.loadFactories(argv[1]);
 const bool local_conflict = a.loadFactories(argv[2]);
 const bool independent = b.loadFactories(argv[2]);
 const auto strategy = b.createStrategy("stable", "unit", "b");
 std::printf("first=%d local_conflict=%d independent=%d strategy=%d\n", first, local_conflict, independent, strategy && strategy->self());
 return first && independent && strategy && strategy->self() ? 0 : 1;
}
