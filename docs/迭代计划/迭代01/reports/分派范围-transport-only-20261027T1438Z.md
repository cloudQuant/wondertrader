# 下一波分派范围（由报表 `coverage-transport-only-20261027T1438Z.txt` 派生，非人工挑选）

- 每路目标 missed 行数：8872, 7977, 7953, 4652, 4610, 4258
- 报表里前 40 个待补文件按 missed 行数排序；同一路只放同一个测试目标的文件。
- **没有测试目标的模块（需要主控先建目标，不能派给 lane）**：WTSTools(1111), Includes(857), WTSTools(616)

## 路 1（预计 8872 行未覆盖）
- 测试目标 `CoverageTests`（主目标，GLOB_RECURSE 自动收新目录）；新目录可用，例如 src/TestUnits/coverage/l01_share_wtsutils/, src/TestUnits/coverage/l02_wtstools_mq/, src/TestUnits/coverage/l03_includes/
  - `src/WtDataStorage/WtRdmDtReader.cpp` missed=2337/2337（现覆盖 0.00%）
  - `src/WtDataStorage/WtDataWriter.cpp` missed=2035/2035（现覆盖 0.00%）
  - `src/WtDataStorage/WtDataReader.cpp` missed=1916/1916（现覆盖 0.00%）
  - `src/WTSTools/WTSDataFactory.cpp` missed=1111/1111（现覆盖 0.00%）
  - `src/Includes/WTSDataDef.hpp` missed=857/888（现覆盖 3.49%）
  - `src/WTSTools/WTSBaseDataMgr.cpp` missed=616/644（现覆盖 4.35%）

## 路 2（预计 7977 行未覆盖）
- 测试目标 `CoverageTests_wtcore`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/n01_wtcore_tradepaths/, src/TestUnits/coverage/n02_wtcore_engines/, src/TestUnits/coverage/p01_wtcore_stractx/, src/TestUnits/coverage/p02_wtcore_engines2/, src/TestUnits/coverage/p11_wtcore_traderadapter/, src/TestUnits/coverage/q01_wtcore3/
  - `src/WtCore/TraderAdapter.cpp` missed=1819/1861（现覆盖 2.26%）
  - `src/WtCore/CtaStraBaseCtx.cpp` missed=1545/1545（现覆盖 0.00%）
  - `src/WtCore/SelStraBaseCtx.cpp` missed=934/934（现覆盖 0.00%）
  - `src/WtCore/WtEngine.cpp` missed=916/916（现覆盖 0.00%）
  - `src/WtCore/HftStraBaseCtx.cpp` missed=659/659（现覆盖 0.00%）
  - `src/WtCore/WtCtaEngine.cpp` missed=571/571（现覆盖 0.00%）
  - `src/WtCore/WtArbiExecuter.cpp` missed=513/513（现覆盖 0.00%）
  - `src/WtCore/WtLocalExecuter.cpp` missed=513/513（现覆盖 0.00%）
  - `src/WtCore/WtDiffExecuter.cpp` missed=507/507（现覆盖 0.00%）

## 路 3（预计 7953 行未覆盖）
- 测试目标 `CoverageTests_btcore`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/l06_wtbtcore/, src/TestUnits/coverage/n04_btcore_mockers/, src/TestUnits/coverage/p03_btcore_mockers2/, src/TestUnits/coverage/p04_btcore_replayer/, src/TestUnits/coverage/p07_btcore_replayer2/, src/TestUnits/coverage/p08_btcore_mockers3/, src/TestUnits/coverage/q02_btcore3/
  - `src/WtBtCore/HisDataReplayer.cpp` missed=3666/3666（现覆盖 0.00%）
  - `src/WtBtCore/CtaMocker.cpp` missed=1611/1611（现覆盖 0.00%）
  - `src/WtBtCore/UftMocker.cpp` missed=987/987（现覆盖 0.00%）
  - `src/WtBtCore/HftMocker.cpp` missed=870/870（现覆盖 0.00%）
  - `src/WtBtCore/SelMocker.cpp` missed=819/819（现覆盖 0.00%）

## 路 4（预计 4652 行未覆盖）
- 测试目标 `CoverageTests_uftcore`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/n03_uftcore_index/, src/TestUnits/coverage/p06_uftcore2/, src/TestUnits/coverage/q04_uftcore3/
  - `src/WtUftCore/TraderAdapter.cpp` missed=1363/1409（现覆盖 3.26%）
  - `src/WtUftCore/UftStraContext.cpp` missed=809/809（现覆盖 0.00%）
- 测试目标 `CoverageTests_btporter`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/q06_btporter/
  - `src/WtBtPorter/WtBtPorter.cpp` missed=846/846（现覆盖 0.00%）
- 测试目标 `CoverageTests_t_TraderMocker`；**用例只能写进这些已登记目录**：（表里没有登记目录 —— 需要主控先加 dir: 条目，lane 不要自造新目录）
  - `src/TraderMocker/TraderMocker.cpp` missed=598/984（现覆盖 39.23%）
- 测试目标 `CoverageTests_t_TraderCTP`；**用例只能写进这些已登记目录**：（表里没有登记目录 —— 需要主控先加 dir: 条目，lane 不要自造新目录）
  - `src/TraderCTP/TraderCTP.cpp` missed=554/1179（现覆盖 53.01%）
- 测试目标 `CoverageTests_exefact`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/n05_exec_factories/, src/TestUnits/coverage/r7_exefact/
  - `src/WtExeFact/WtStockVWapExeUnit.cpp` missed=482/482（现覆盖 0.00%）

## 路 5（预计 4610 行未覆盖）
- 测试目标 `CoverageTests_dtcore`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/q05_dtsrv/
  - `src/WtDtHelper/WtDtHelper.cpp` missed=883/883（现覆盖 0.00%）
  - `src/WtDtCore/StateMonitor.cpp` missed=611/611（现覆盖 0.00%）
  - `src/WtDtCore/UDPCaster.cpp` missed=476/476（现覆盖 0.00%）
- 测试目标 `CoverageTests_dtservo`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/q05b_dtservo/
  - `src/WtDtServo/WtDtRunner.cpp` missed=500/500（现覆盖 0.00%）
  - `src/WtDtServo/WtDataManager.cpp` missed=475/475（现覆盖 0.00%）
- 测试目标 `CoverageTests_t_CTPLoader`；**用例只能写进这些已登记目录**：（表里没有登记目录 —— 需要主控先加 dir: 条目，lane 不要自造新目录）
  - `src/CTPLoader/TraderSpi.cpp` missed=603/673（现覆盖 10.40%）
- 测试目标 `CoverageTests_mqshare`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/n06_strategies_mq_share/, src/TestUnits/coverage/r19_mq/
  - `src/WtShareHelper/ShareBlocks.cpp` missed=571/571（现覆盖 0.00%）
- 测试目标 `CoverageTests_m_WtRunner`；**用例只能写进这些已登记目录**：（表里没有登记目录 —— 需要主控先加 dir: 条目，lane 不要自造新目录）
  - `src/WtRunner/WtRunner.cpp` missed=491/491（现覆盖 0.00%）

## 路 6（预计 4258 行未覆盖）
- 测试目标 `CoverageTests_porter`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/q06_porter/
  - `src/WtPorter/WtRtRunner.cpp` missed=938/938（现覆盖 0.00%）
  - `src/WtPorter/WtPorter.cpp` missed=848/848（现覆盖 0.00%）
- 测试目标 `CoverageTests_adstorage`；**用例只能写进这些已登记目录**：src/TestUnits/coverage/p05_datastorage2/
  - `src/WtDataStorageAD/WtDataWriterAD.cpp` missed=878/878（现覆盖 0.00%）
  - `src/WtDataStorageAD/WtDataReaderAD.cpp` missed=514/514（现覆盖 0.00%）
- 测试目标 `CoverageTests_t_TraderCTPOpt`；**用例只能写进这些已登记目录**：（表里没有登记目录 —— 需要主控先加 dir: 条目，lane 不要自造新目录）
  - `src/TraderCTPOpt/TraderCTPOpt.cpp` missed=583/1374（现覆盖 57.57%）
- 测试目标 `CoverageTests_t_TraderYD`；**用例只能写进这些已登记目录**：（表里没有登记目录 —— 需要主控先加 dir: 条目，lane 不要自造新目录）
  - `src/TraderYD/TraderYD.cpp` missed=497/876（现覆盖 43.26%）
