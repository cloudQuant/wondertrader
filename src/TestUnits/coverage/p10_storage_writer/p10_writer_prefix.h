// p10 专用前置 include 收口（不是测试用例，只为安全暴露 WtDataWriter 私有成员）
//
// 必须先把第三方/基础头在 #define private public 之前包含干净：
// Includes/FasterDefs.h 会拉入 FasterLibs/ankerl/unordered_dense.h，那里的类内成员
// 先以「类默认 private」声明、再以（被宏改写成 public 的）显式标签定义，
// 直接暴露给宏就是 'iter_t' redeclared with 'public' access 编译错误（实测）。
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSStruct.h"
#include "Includes/WTSObject.hpp"
#include "Includes/FasterDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IDataWriter.h"
#include "Share/BoostFile.hpp"
#include "Share/StrUtil.hpp"
#include "Share/TimeUtils.hpp"
#include "WTSUtils/WTSCmpHelper.hpp"
#include "WtDataStorage/DataDefine.h"

// 覆盖率需要直接驱动私有成员方法与成员变量（本仓库先例：p03）
#define private public
#define protected public
#include "WtDataStorage/WtDataWriter.h"
#undef private
#undef protected
