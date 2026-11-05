#include <thread>
int main() { int value = 0; std::thread t([&] { value = 42; }); t.join(); return value == 42 ? 0 : 1; }
