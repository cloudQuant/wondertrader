#include "WtDtCore/StateMonitor.h"
#include <chrono>
#include <thread>
int main() {
    for (int i = 0; i < 10; ++i) {
        StateMonitor monitor;
        monitor.run();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        monitor.stop();
    }
    return 0;
}
