#include "WtDataStorageAD/LMDBKeys.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string exchg(std::strtoul(argv[1], nullptr, 10), 'E');
    const std::string code(std::strtoul(argv[2], nullptr, 10), 'C');
    LMDBBarKey bar(exchg.c_str(), code.c_str(), 0x01020304);
    LMDBHftKey tick(exchg.c_str(), code.c_str(), 20240101, 93000000);
    bool be=std::memchr(bar._exchg, 0, sizeof(bar._exchg)) != nullptr;
    bool bc=std::memchr(bar._code, 0, sizeof(bar._code)) != nullptr;
    bool he=std::memchr(tick._exchg, 0, sizeof(tick._exchg)) != nullptr;
    bool hc=std::memchr(tick._code, 0, sizeof(tick._code)) != nullptr;
    std::cout << "bar.exchg=" << be << " bar.code=" << bc << " tick.exchg=" << he << " tick.code=" << hc << '\n';
    return be && bc && he && hc ? 0 : 1;
}
