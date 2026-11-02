#include "src/Share/CodeHelper.hpp"
#include <iostream>

int main() {
    const char* codes[] = {"ag1912", "au912", "ag12-4", "a12345", "BTC202412", "BTC202499"};
    for (const char* code : codes) {
        const auto converted = CodeHelper::rawMonthCodeToStdCode(code, "SHFE");
        std::cout << code << " monthly=" << CodeHelper::isMonthlyCode(code)
                  << " => " << converted << " size=" << converted.size() << '\n';
    }
}
