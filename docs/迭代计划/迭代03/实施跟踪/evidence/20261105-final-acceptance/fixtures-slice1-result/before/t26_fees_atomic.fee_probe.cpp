#include "CTPLoader/TraderSpi.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <sys/resource.h>
#include <signal.h>

CThostFtdcTraderApi* pUserApi = nullptr;
std::string FRONT_ADDR, BROKER_ID, INVESTOR_ID, PASSWORD, SAVEPATH, APPID, AUTHCODE;
std::string COMM_FILE, CONT_FILE, FEES_FILE;
uint32_t CLASSMASK = 1;
bool ONLYINCFG = false, INCREMENTAL = false, QRYFEES = false;
int iRequestID = 0;
std::map<std::string, std::string> MAP_NAME, MAP_SESSION;
std::set<std::string> SET_FILTERS;
extern loader::FeeMap _fees;
extern loader::MarginMap _margins;
extern loader::ContractMap _contracts;
static int fault = 0;
bool wtT26FeesFailIo(int stage) { return fault == stage; }
struct CTraderSpiQueueTestAccess {
  static void add(CTraderSpi& spi) {
    std::lock_guard<std::mutex> lock(spi._mtx); spi._queries.push([] { return true; });
  }
  static void clear(CTraderSpi& spi) {
    std::lock_guard<std::mutex> lock(spi._mtx); spi._queries.pop();
  }
  static void load(CTraderSpi& spi) { spi.LoadFromJson(); }
  static void tradingDay(CTraderSpi& spi, int day) { spi._trading_day=day; }
#ifdef WT_T26_PROBE_ARM_REQUESTS
  // 2026-11-04: the current CTPLoader (T26-011 request tracking + card 14, 14c=C) accepts
  // fee callbacks only for an in-flight request ID while QRYFEES is set, and fails a
  // request whose terminal frame arrives without any payload row. Register exactly the
  // IDs this probe injects (sent and SDK-returned, as BeginFeeQuery/FinishFeeSubmission
  // would). Only the current-code builds define this macro; the baseline/unit-baseline
  // red controls compile the original probe flow unchanged.
  static void arm(CTraderSpi& spi, int id, bool commission, bool sealed, const char* code) {
    std::lock_guard<std::mutex> lock(spi._mtx);
    spi._fee_batch_active=true; spi._fee_batch_sealed=sealed;
    CTraderSpi::FeeQueryState state{};
    state.kind=commission ? CTraderSpi::FeeQueryKind::Commission : CTraderSpi::FeeQueryKind::Margin;
    state.callReturned=true;
    state.deadline=std::chrono::steady_clock::now()+std::chrono::minutes(1);
    state.code=code;
    spi._fee_requests.emplace(id,std::move(state));
  }
#endif
};

template<size_t N> void set(char (&out)[N], const char* value) {
  std::strncpy(out, value, N-1); out[N-1] = '\0';
}
void instrument(CTraderSpi& spi, const char* code, const char* product) {
  CThostFtdcInstrumentField f{};
  set(f.InstrumentID, code); set(f.ProductID, product); set(f.ExchangeID, "SHFE");
  set(f.InstrumentName, code); set(f.OpenDate, "20170101"); set(f.ExpireDate, "20251231");
  f.ProductClass = THOST_FTDC_PC_Futures; f.VolumeMultiple = 10; f.PriceTick = 1;
  spi.OnRspQryInstrument(&f, nullptr, 1, false);
}
void commission(CTraderSpi& spi, const char* code, double value) {
  CThostFtdcInstrumentCommissionRateField f{};
  set(f.InstrumentID, code); f.OpenRatioByVolume=value; f.CloseRatioByVolume=value;
  f.CloseTodayRatioByVolume=value; spi.OnRspQryInstrumentCommissionRate(&f,nullptr,2,false);
}
void margin(CTraderSpi& spi, const char* code, double value, int id=3, bool last=false) {
  CThostFtdcInstrumentMarginRateField f{};
  set(f.InstrumentID,code); f.LongMarginRatioByMoney=value; f.ShortMarginRatioByMoney=value;
  spi.OnRspQryInstrumentMarginRate(&f,nullptr,id,last);
}
int main(int argc,char** argv) {
  if(argc!=3) return 99;
  const std::string mode=argv[1]; SAVEPATH=std::string(argv[2])+"/";
  FEES_FILE="fees.json"; COMM_FILE="commodities.json"; CONT_FILE="contracts.json";
  if(mode=="absolute") FEES_FILE=SAVEPATH+"fees.json";
  if(mode=="openfail") FEES_FILE="missing-dir/fees.json";
  CTraderSpi spi;
  if(mode.rfind("loaded_",0)==0) {
    INCREMENTAL=true; CTraderSpiQueueTestAccess::load(spi);
    if(_contracts.size()!=1 || _contracts.count("SR2510")!=1) return 95;
    if(mode=="loaded_duplicate") {
      CTraderSpiQueueTestAccess::tradingDay(spi,20250102);
      CThostFtdcInstrumentField f{};
      set(f.InstrumentID,"SR510"); set(f.ProductID,"SR"); set(f.ExchangeID,"CZCE");
      set(f.InstrumentName,"sugar"); set(f.OpenDate,"20170101"); set(f.ExpireDate,"20251231");
      f.ProductClass=THOST_FTDC_PC_Futures; f.VolumeMultiple=10; f.PriceTick=1;
      spi.OnRspQryInstrument(&f,nullptr,1,false);
      if(_contracts.size()!=2) return 94;
    }
    if(mode=="loaded_alias_conflict") {
      auto other=_contracts["SR2510"]; other.m_strCode="SR2610";
      _contracts["SR2610"]=other;
    }
    commission(spi,mode=="loaded_mixed"?"SR2510":mode=="loaded_product"?"SR":"SR510",0.25);
    if(mode=="loaded_fee_conflict") commission(spi,"SR2510",0.50);
    margin(spi,"SR510",0.15);
    if(mode=="loaded_duplicate_margin") margin(spi,"SR2510",0.15);
    QRYFEES=true;
#ifdef WT_T26_PROBE_ARM_REQUESTS
    // 14c: request 4 carries one row (identical to the staged SR510 margin) before the
    // original empty terminal frame, so the batch is not an empty response.
    CTraderSpiQueueTestAccess::arm(spi,4,false,true,"SR510");
    margin(spi,"SR510",0.15,4,false);
#endif
    spi.OnRspQryInstrumentMarginRate(nullptr,nullptr,4,true);
    return 97;
  }
  instrument(spi,"rb2510","rb"); instrument(spi,"au2512","au");
  if(_contracts.size()!=2) { std::cerr << "instrument setup failed\n"; return 98; }
  commission(spi,"rb2510",0.0); commission(spi,"au",-0.25);
#ifdef WT_T26_PROBE_ARM_REQUESTS
  // filter publishes rb2510 from the terminal row (0.20): stage the same value, because the
  // current code rejects two different values for one key (no assertion reads this margin).
  margin(spi,"rb2510",mode=="filter"?0.20:0.10); margin(spi,"au2512",0.20);
#else
  margin(spi,"rb2510",0.10); margin(spi,"au2512",0.20);
#endif
  if(mode.rfind("units_",0)==0 || mode.rfind("nan_field_",0)==0 || mode.rfind("sentinel_fee_",0)==0) {
    CThostFtdcInstrumentCommissionRateField f{}; set(f.InstrumentID,"rb2510");
    if(mode=="units_mixed_open_volume") { f.OpenRatioByVolume=1; f.CloseRatioByMoney=0.01; }
    if(mode=="units_mixed_open_money") { f.OpenRatioByMoney=0.01; f.CloseRatioByVolume=1; }
    if(mode=="units_mixed_same") { f.OpenRatioByMoney=0.01; f.OpenRatioByVolume=1; }
    if(mode=="units_mixed_tiny") { f.OpenRatioByMoney=5e-7; f.OpenRatioByVolume=9; }
    if(mode=="units_tiny_money") f.OpenRatioByMoney=5e-7;
    if(mode.rfind("nan_field_",0)==0) {
      int index=std::stoi(mode.substr(10));
      if(index<3) f.OpenRatioByVolume=1; else f.OpenRatioByMoney=0.01;
      double* fields[]={&f.OpenRatioByMoney,&f.CloseRatioByMoney,&f.CloseTodayRatioByMoney,
                        &f.OpenRatioByVolume,&f.CloseRatioByVolume,&f.CloseTodayRatioByVolume};
      *fields[index]=std::numeric_limits<double>::quiet_NaN();
    }
    if(mode.rfind("sentinel_fee_",0)==0) {
      int index=mode.back()-'0';
      if(index<3) f.OpenRatioByVolume=1; else f.OpenRatioByMoney=0.01;
      double* fields[]={&f.OpenRatioByMoney,&f.CloseRatioByMoney,&f.CloseTodayRatioByMoney,
                        &f.OpenRatioByVolume,&f.CloseRatioByVolume,&f.CloseTodayRatioByVolume};
      *fields[index]=mode.find("_d_")!=std::string::npos ? std::numeric_limits<double>::max()
        : static_cast<double>(std::numeric_limits<float>::max());
    }
    spi.OnRspQryInstrumentCommissionRate(&f,nullptr,6,false);
  }
  // rb sorts after au, so failures occur after a valid row has been staged.
  if(mode=="missing_fee") _fees.erase("rb2510");
  if(mode=="missing_contract") _contracts.erase("rb2510");
  if(mode=="nan_fee") _fees["rb2510"]._open=std::numeric_limits<double>::quiet_NaN();
  if(mode=="nan_margin") _margins["rb2510"]._long=std::numeric_limits<double>::infinity();
  if(mode=="empty_data") _margins.clear();
  if(mode=="missing_margin") _margins.erase("rb2510");
  if(mode=="filter") { SET_FILTERS.insert("rb"); _margins.erase("au2512"); _fees.erase("au"); }
  if(mode=="filter_unselected_invalid") { SET_FILTERS.insert("rb"); _contracts["au2512"].m_strExchg.clear(); }
  if(mode=="empty_exchange") _contracts["rb2510"].m_strExchg.clear();
  if(mode=="empty_code") _contracts["rb2510"].m_strCode.clear();
  if(mode=="empty_product") _contracts["rb2510"].m_strProduct.clear();
  if(mode=="duplicate_key") _contracts["rb2510"].m_strCode=_contracts["au2512"].m_strCode;
  if(mode=="commission_error") {
    _fees.erase("rb2510"); CThostFtdcRspInfoField err{}; err.ErrorID=7;
    spi.OnRspQryInstrumentCommissionRate(nullptr,&err,5,true);
  }
  if(mode.rfind("margin_",0)==0 || mode.rfind("sentinel_margin_",0)==0) {
    CThostFtdcInstrumentMarginRateField f{}; set(f.InstrumentID,"rb2510");
    f.LongMarginRatioByMoney=0.10; f.ShortMarginRatioByMoney=0.10;
    if(mode=="margin_zero") { f.LongMarginRatioByMoney=0; f.ShortMarginRatioByMoney=0; }
    if(mode=="margin_different_short") f.ShortMarginRatioByMoney=0.13;
    if(mode=="margin_long_volume") f.LongMarginRatioByVolume=2;
    if(mode=="margin_short_volume") f.ShortMarginRatioByVolume=2;
    if(mode.rfind("margin_nan_",0)==0) {
      int index=std::stoi(mode.substr(11));
      double* fields[]={&f.LongMarginRatioByMoney,&f.ShortMarginRatioByMoney,
                        &f.LongMarginRatioByVolume,&f.ShortMarginRatioByVolume};
      *fields[index]=std::numeric_limits<double>::quiet_NaN();
    }
    if(mode.rfind("sentinel_margin_",0)==0) {
      int index=mode.back()-'0';
      double* fields[]={&f.LongMarginRatioByMoney,&f.ShortMarginRatioByMoney,
                        &f.LongMarginRatioByVolume,&f.ShortMarginRatioByVolume};
      *fields[index]=mode.find("_d_")!=std::string::npos ? std::numeric_limits<double>::max()
        : static_cast<double>(std::numeric_limits<float>::max());
    }
    spi.OnRspQryInstrumentMarginRate(&f,nullptr,7,false);
  }
  if(mode=="write") fault=1; if(mode=="flush") fault=2;
  if(mode=="close") fault=3; if(mode=="rename") fault=4;
  if(mode=="disk_limit") {
    signal(SIGXFSZ, SIG_IGN); struct rlimit limit{1,1};
    if(setrlimit(RLIMIT_FSIZE,&limit)!=0) return 96;
  }
  QRYFEES=true;
  if(mode=="earlier_error" || mode=="earlier_margin_error") {
    CTraderSpiQueueTestAccess::add(spi);
#ifdef WT_T26_PROBE_ARM_REQUESTS
    CTraderSpiQueueTestAccess::arm(spi,8,mode=="earlier_error",false,"");
#endif
    CThostFtdcRspInfoField err{}; err.ErrorID=8;
    if(mode=="earlier_error") spi.OnRspQryInstrumentCommissionRate(nullptr,&err,8,true);
    else spi.OnRspQryInstrumentMarginRate(nullptr,&err,8,true);
    CTraderSpiQueueTestAccess::clear(spi);
  }
#ifdef WT_T26_PROBE_ARM_REQUESTS
  CTraderSpiQueueTestAccess::arm(spi,4,mode=="response_error",true,mode=="filter"?"rb2510":"au2512");
#endif
  if(mode=="response_error") {
    CThostFtdcRspInfoField err{}; err.ErrorID=7;
    spi.OnRspQryInstrumentCommissionRate(nullptr,&err,4,true);
  } else if (mode=="empty_terminal" || mode=="empty_data") {
    // A successful terminal empty payload is legal after earlier records.
#ifdef WT_T26_PROBE_ARM_REQUESTS
    // 14c: the earlier record must belong to the same request. Deliver one non-terminal
    // row identical to the staged au2512 margin; empty_data then re-empties the margin
    // table so DumpFees still sees no margin data, as before.
    margin(spi,"au2512",0.20,4,false);
    if(mode=="empty_data") _margins.clear();
#endif
    spi.OnRspQryInstrumentMarginRate(nullptr,nullptr,4,true);
  } else {
    CThostFtdcInstrumentMarginRateField f{};
    set(f.InstrumentID,mode=="filter"?"rb2510":"au2512"); f.LongMarginRatioByMoney=0.20; f.ShortMarginRatioByMoney=0.20;
    spi.OnRspQryInstrumentMarginRate(&f,nullptr,4,true);
  }
  return 97;
}
