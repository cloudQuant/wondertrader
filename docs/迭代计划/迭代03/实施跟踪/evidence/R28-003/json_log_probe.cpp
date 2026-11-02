#include "WtRunner/WtRunner.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

int main()
{
    const std::string logcfg = "probe-logcfg.yaml";
    { std::ofstream f(logcfg); f << "root:\n    async: false\n    level: error\n    sinks:\n    -   filename: Logs/probe_%s.log\n        pattern: '[%Y.%m.%d %H:%M:%S - %-5l] %v'\n        truncate: true\n        type: daily_file_sink\n"; if (!f.good()) return 2; }
    const std::string broken = std::filesystem::absolute("probe-invalid-config.json").string();
    { std::ofstream f(broken); f << "{ \"basefiles\": ][ bad json "; if (!f.good()) return 3; }
    auto runner = std::make_unique<WtRunner>();
    runner->init(logcfg);
    const bool result = runner->config(broken);
    std::cout << "input=" << broken << "\nresult=" << (result ? "true" : "false") << std::endl;
    return result ? 1 : 0;
}
