#include "WtDtHelper/WtDtHelper.h"
#include "Includes/WTSDataDef.hpp"
#include <filesystem>
#include <cstring>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    namespace fs = std::filesystem; fs::path root(argv[1]);
    for (int tick=0; tick!=2; ++tick) {
        auto in = root / (tick ? "tin" : "bin"); auto out = root / (tick ? "tout" : "bout");
        fs::create_directories(in); fs::create_directories(out); auto src=(in / "one.dsb").string();
        if (tick) {
            WTSTickStruct v{}; std::strncpy(v.exchg,"SHFE",sizeof(v.exchg)-1); std::strncpy(v.code,"rb2010",sizeof(v.code)-1);
            v.action_date=v.trading_date=20200116; v.action_time=93000; v.price=3000;
            if (!store_ticks(src.c_str(),&v,1,nullptr)) return 3; dump_ticks(in.c_str(),out.c_str(),"",nullptr);
        } else {
            WTSBarStruct v{}; v.date=20200116; v.open=3000; v.close=3001;
            if (!store_bars(src.c_str(),&v,1,"d",nullptr)) return 4; dump_bars(in.c_str(),out.c_str(),"",nullptr);
        }
        if (!fs::exists(out / "one.csv")) return 5;
        fs::copy_file(out / "one.csv",root / (tick ? "tick.csv" : "bar.csv"));
    }
}
