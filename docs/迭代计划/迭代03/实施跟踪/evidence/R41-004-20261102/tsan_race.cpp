#include <thread>
int value = 0;
int main() { std::thread t([] { value = 1; }); value = 2; t.join(); return 0; }
