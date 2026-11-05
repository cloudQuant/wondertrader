#pragma once
#include <cstdlib>
#include <string>
class WtRunner { public: void init(const std::string&) {} bool config(const std::string&) { return true; } void run(bool) {} bool parserCommandsSubmitted() const { return std::getenv("I03_SUBMITTED")[0] == '1'; } bool runSucceeded() const { return std::getenv("I03_RUN_OK")[0] == '1'; } };
