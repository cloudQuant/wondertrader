#include <thread>
int value = 0;
int main() { std::thread t([] { for (int i = 0; i < 100000; ++i) ++value; }); for (int i = 0; i < 100000; ++i) ++value; t.join(); return 0; }
