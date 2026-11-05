# BUG-R12-13：DataManager 宿主交易日改 checked 映射；收盘转存对 0 日期拒绝

- `src/WtDtCore/DataManager.cpp` 宿主 `getTradingDate` 改用 `WTSBaseDataMgr::checkedGetTradingDate`：缺日历/品种或时钟不可映射时记错误返回 0（旧接口兜底返回“今天”）。
- `src/WtDataStorage/WtDataWriter.cpp` 收盘转存取到 0 日期时记错误并跳过该合约转存，不写 0 日期历史文件（AD 写端不经此接口）。
- 验证：仅回归（CoverageTests/dtcore/dtservo/i03_dtporter_start 逐例除既有 Q05 外全部通过，`regression-summary.json`）；专门红绿需已初始化写端模块的宿主夹具，未做。改前/改后副本见 `before/`、`after/`。
