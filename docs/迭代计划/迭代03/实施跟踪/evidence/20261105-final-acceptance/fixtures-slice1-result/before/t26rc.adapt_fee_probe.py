from pathlib import Path
import sys
source = Path('src/TestUnits/fixtures/t26_fees_atomic/fee_probe.cpp').read_text()
needle = '  static void tradingDay(CTraderSpi& spi, int day) { spi._trading_day=day; }'
addition = '''  static void arm(CTraderSpi& spi, int id, bool commission, bool sealed, const char* code="") {
    std::lock_guard<std::mutex> lock(spi._mtx);
    spi._fee_batch_active=true; spi._fee_batch_sealed=sealed;
    const auto kind=commission ? CTraderSpi::FeeQueryKind::Commission : CTraderSpi::FeeQueryKind::Margin;
    CTraderSpi::FeeQueryState state{kind,true,false,std::chrono::steady_clock::now()+std::chrono::minutes(1)};
    state.code=code; spi._fee_requests.emplace(id,std::move(state));
  }
'''
assert source.count(needle)==1
source=source.replace(needle,needle+'\n'+addition)
needle='QRYFEES=true; spi.OnRspQryInstrumentMarginRate(nullptr,nullptr,4,true);'
assert source.count(needle)==1
source=source.replace(needle,'QRYFEES=true; CTraderSpiQueueTestAccess::arm(spi,4,false,true); spi.OnRspQryInstrumentMarginRate(nullptr,nullptr,4,true);')
needle='    CTraderSpiQueueTestAccess::add(spi);'
assert source.count(needle)==1
source=source.replace(needle,needle+'\n    CTraderSpiQueueTestAccess::arm(spi,8,mode=="earlier_error",false);')
needle='  if(mode=="response_error") {'
assert source.count(needle)==1
source=source.replace(needle,'  CTraderSpiQueueTestAccess::arm(spi,4,mode=="response_error",true,mode=="filter"?"rb2510":"au2512");\n'+needle)
needle='margin(spi,"rb2510",0.10); margin(spi,"au2512",0.20);'
assert source.count(needle)==1
source=source.replace(needle,'margin(spi,"rb2510",mode=="filter"?0.20:0.10); margin(spi,"au2512",0.20);')
Path(sys.argv[1]).write_text(source)
source=Path('src/TestUnits/fixtures/t26_fees_atomic/run_tests.py').read_text()
needle="\nfor mode in ('units_mixed_open_volume'"
assert source.count(needle)==1
Path(sys.argv[2]).write_text(source.split(needle)[0]+'\n')
