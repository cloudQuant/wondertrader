#include "Includes/WTSSessionInfo.hpp"
#include <cstdint>
#include <iostream>
int main() { int32_t o; uint32_t d,t; while (std::cin >> o >> d >> t) {auto* s=wtp::WTSSessionInfo::create("id","test",o); uint32_t result=0xabcdefu; bool ok=s->tryGetOffsetDate(d,t,result); std::cout << ok << ' ' << result << ' ' << s->offsetTime(t,true) << ' ' << s->offsetTime(t,false) << '\n'; s->release();} }
