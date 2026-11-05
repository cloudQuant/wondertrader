#include "../../../WtDataStorageAD/WtBtDtReaderAD.h"
#include "../../../WtDataStorageAD/WtDataReaderAD.h"
#include "../../../WtDataStorageAD/WtRdmDtReaderAD.h"
#include "../../../WtDataStorageAD/WtDataWriterAD.h"
#include "../../../Includes/WTSContractInfo.hpp"
#include "../../../Includes/WTSDataDef.hpp"
#include <cassert>
#include <string>

int main()
{
    const std::string longExchg(16, 'E');
    const std::string longCode(32, 'C');
    wtp::WtBtDtReaderAD bt;
    std::string buffer("unchanged");
    for (const char* ex : {static_cast<const char*>(nullptr), longExchg.c_str()}) {
        assert(!bt.read_raw_bars(ex, "c", KP_DAY, buffer));
        assert(!bt.read_raw_ticks(ex, "c", 20240101, buffer));
    }
    for (const char* code : {static_cast<const char*>(nullptr), longCode.c_str()}) {
        assert(!bt.read_raw_bars("EX", code, KP_DAY, buffer));
        assert(!bt.read_raw_ticks("EX", code, 20240101, buffer));
    }
    assert(buffer == "unchanged");
    const std::string bad = longExchg + ".c";
    wtp::WtDataReaderAD reader;
    wtp::WtRdmDtReaderAD rdm;
    for (const char* code : {static_cast<const char*>(nullptr), bad.c_str()}) {
        assert(reader.readTickSlice(code, 1, 20240101093000000ULL) == nullptr);
        assert(reader.readKlineSlice(code, KP_DAY, 1, 202401010930ULL) == nullptr);
        assert(rdm.readTickSliceByCount(code, 1, 20240101093000000ULL) == nullptr);
        assert(rdm.readTickSliceByRange(code, 20240101093000000ULL, 20240101093100000ULL) == nullptr);
        assert(rdm.readTickSliceByDate(code, 20240101) == nullptr);
        assert(rdm.readKlineSliceByCount(code, KP_DAY, 1, 202401010930ULL) == nullptr);
        assert(rdm.readKlineSliceByRange(code, KP_DAY, 202401010930ULL, 202401010931ULL) == nullptr);
    }

    // The generic contract permits an exchange too wide for an AD LMDB key.
    // Admission must reject it before retaining the tick or touching writer state.
    WTSContractInfo* contract = WTSContractInfo::create("c", "n", longExchg.c_str(), "p");
    assert(contract != nullptr);
    WTSTickData* tick = WTSTickData::create("c");
    assert(tick != nullptr);
    tick->setContractInfo(contract);
    WtDataWriterAD writer;
    const auto references = tick->retainCount();
    assert(!writer.writeTick(tick, 0));
    assert(tick->retainCount() == references);
    tick->release();
    contract->release();

    contract = WTSContractInfo::create("c", "n", "EX", "p");
    assert(contract != nullptr);
    WTSTickStruct raw{};
    std::memset(raw.exchg, 'E', sizeof(raw.exchg));
    raw.code[0] = 'c';
    tick = WTSTickData::create(raw);
    assert(tick != nullptr);
    tick->setContractInfo(contract);
    assert(!writer.writeTick(tick, 0));
    assert(tick->retainCount() == 1);
    tick->release();
    contract->release();
}
