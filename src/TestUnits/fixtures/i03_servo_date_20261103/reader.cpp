#include <Includes/IRdmDtReader.h>
#include <Includes/WTSDataDef.hpp>
#include <cstdint>
namespace {
unsigned rangeCalls=0, countCalls=0; uint64_t startTime=0, endTime=0;
wtp::WTSBarStruct bar;
class SpyReader final : public wtp::IRdmDtReader {
public:
    wtp::WTSOrdDtlSlice* readOrdDtlSliceByRange(const char*,uint64_t,uint64_t) override { return nullptr; }
    wtp::WTSOrdQueSlice* readOrdQueSliceByRange(const char*,uint64_t,uint64_t) override { return nullptr; }
    wtp::WTSTransSlice* readTransSliceByRange(const char*,uint64_t,uint64_t) override { return nullptr; }
    wtp::WTSTickSlice* readTickSliceByDate(const char*,uint32_t) override { return nullptr; }
    wtp::WTSTickSlice* readTickSliceByRange(const char*,uint64_t,uint64_t) override { return nullptr; }
    wtp::WTSKlineSlice* readKlineSliceByRange(const char* code,wtp::WTSKlinePeriod p,uint64_t start,uint64_t end) override {
        ++rangeCalls; startTime=start; endTime=end; return wtp::WTSKlineSlice::create(code,p,1,&bar,1);
    }
    wtp::WTSTickSlice* readTickSliceByCount(const char*,uint32_t,uint64_t) override { return nullptr; }
    wtp::WTSKlineSlice* readKlineSliceByCount(const char*,wtp::WTSKlinePeriod,uint32_t,uint64_t) override { ++countCalls; return nullptr; }
};
}
extern "C" wtp::IRdmDtReader* createRdmDtReader() { return new SpyReader; }
extern "C" void deleteRdmDtReader(wtp::IRdmDtReader* reader) { delete reader; }
extern "C" void spy_reset() { rangeCalls=0; countCalls=0; startTime=endTime=0; }
extern "C" unsigned spy_range_calls() { return rangeCalls; }
extern "C" unsigned spy_count_calls() { return countCalls; }
extern "C" uint64_t spy_start() { return startTime; }
extern "C" uint64_t spy_end() { return endTime; }
