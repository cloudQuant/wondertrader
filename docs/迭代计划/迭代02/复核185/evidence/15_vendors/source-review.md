# 15_vendors static review evidence

- Source review only; no command executed a test binary or built a target in this subtask.
- Every `source_evidence` and `test_evidence` location is recorded per row in `triage/15_vendors.json`.
- CTPMini version check: `src/API/CTPMini1.5.8/` contains four API headers plus Linux/Windows libraries and no local release note/query callback contract. `ThostFtdcTraderApi.h:101` gives the callback signature only. The SimNow CTPIIMini API Ver1.2 PDF mentioned by the parent was not treated as evidence for CTPMini1.5.8 because the repository contains no version correspondence declaration.
- Date field check: the CTPMini header defines the depth market data `TradingDay` and `ActionDay` as separate `TThostFtdcDateType` members (the vendor header is GBK encoded; comments identify ActionDay as business date). The three R105 adapters use host time to correct ActionDay and have the same unreachable 00-hour branch.
- XTP date check: pinned `XTP_TRADING_DAY_LEN` is 9; the API comment says the trading day is correct only after successful login. No exact date-format statement was found in that pinned SDK, so overlength fake strings are rejected as positive-test inputs without asserting a replacement vendor format.
- T3-014 dataflow check: config fields reach CTPMini/Femas SDK requests via unbounded `strcpy`; pinned array declarations define byte capacities. Safe tests should exercise exact maximum and maximum+1 through validation/spy boundaries, never by writing the overlength input into a fixed field.
- Readiness: parent reported refreshed CoverageTests targets; this subtask did not execute any of them. All row evidence levels therefore remain STATIC/PENDING, not LOCAL_REPRO.
