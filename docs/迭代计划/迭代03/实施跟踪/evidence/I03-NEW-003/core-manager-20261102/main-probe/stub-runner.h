#pragma once
#include <cstdlib>
#include <cstdio>
#include <string>
class WtRunner {
public:
 void init(const std::string&) {}
 bool config(const std::string&) { return true; }
 void run(bool async) { std::puts(async ? "async" : "sync-returned"); }
 bool parserCommandsSubmitted() const { const char* s=std::getenv("I03_TEST_SUBMITTED"); return s && s[0]=='1'; }
};
