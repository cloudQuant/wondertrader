#include "Share/CodeHelper.hpp"
#include <iostream>
int main() {
    auto code = CodeHelper::extractStdCode("SHFE.rb.2510", nullptr);
    std::cout << code.stdCommID() << '\n';
    return std::string(code.stdCommID()) == "SHFE.rb" ? 0 : 1;
}
