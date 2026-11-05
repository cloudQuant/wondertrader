# R132-001 待主代理应用的读端补丁（文件被 slice4-result 锁定，slice2 未改）

`src/WtDataStorage/WtDataReader.cpp`、`src/WtDataStorage/WtRdmDtReader.cpp` 在本轮被 slice4-result（I03-NEW-083）锁定。
解压预算在 WtDataStorage 动态库内按模块生效：WtBtDtReader::init 与 WtDataWriter::init 已接入（slice2），
但实盘读端 WtDataReader / 数据服务读端 WtRdmDtReader 单独使用时仍无法由宿主配置。锁释放后按以下方式补齐
（与 WtBtDtReader 相同；键与 path 同级，非法值 fail-closed）：

1. 两个文件在 `#include "../WTSUtils/WTSCmpHelper.hpp"` 之后追加：
   `#include "../WTSUtils/WTSCmpBudgetConfig.hpp"`
2. `WtDataReader::init`：在 `cfg == NULL` 判断之后、读取 `path` 之前插入：
   ```cpp
   std::string budgetError;
   if (wt_cmp_budget::apply(cfg, budgetError) == wt_cmp_budget::ApplyResult::Invalid)
       pipe_reader_log(sink, LL_ERROR, "{}; decompression in this storage module is refused", budgetError);
   ```
3. `WtRdmDtReader::init`：在 `if (cfg == NULL) return;` 之后插入同样三行（日志函数为 `pipe_rdmreader_log(_sink, ...)`）。
4. 用例：仿照 `l04_datastorage/i03_r132_btreader_budget_test.cpp`，对 WtDataReader（readKlineSlice 历史块）与
   WtRdmDtReader（readKlineSliceByRange/历史 HFT 块）各补“小预算拒绝 + 非法值 fail-closed + 缺省可读”三例；
   红控为改前对象重链（配置被忽略）。
