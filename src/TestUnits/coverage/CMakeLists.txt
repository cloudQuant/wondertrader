# 覆盖率专用测试目标：仅在 WT_ENABLE_COVERAGE=ON 时构建
#
# 为什么要拆成多个可执行文件（三条都是本轮实测，不是推测）：
#  BUG-L00-002  插件是 SHARED + hidden 可见性，C++ 内部符号本来链不上 → 只能把 TU 编进测试二进制；
#  BUG-L00-003  两个插件的源码不能进同一个二进制（WtHelper.cpp / WtStraFact 被多目录复制，
#               实测 34 个 duplicate symbol）；
#  BUG-L00-005  libWtCore.a 与 libWtUftCore.a 都定义 wtp::ActionPolicyMgr::init 这个强符号，
#               静态库惰性抽取会**静默**只取一份，另一份实现永不进二进制 → 模块归因是假的。
# 因此：每个核心/插件族 = 一个测试可执行文件，只编自己那一族的生产 TU，
# 且核心型目标**不链接兄弟核心的静态库**（兄弟核心的符号由被编进来的那批 TU 自己满足）。
IF(NOT WT_ENABLE_COVERAGE)
	RETURN()
ENDIF()

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

file(GLOB_RECURSE COV_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
IF(NOT COV_SRCS)
	file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/placeholder_test.cpp"
		"#include <gtest/gtest.h>\nTEST(coverage_harness, placeholder) { SUCCEED(); }\n")
	file(GLOB_RECURSE COV_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
ENDIF()

# I03-NEW-015：coverage/ 下自带 CMakeLists.txt 的子目录是独立 CMake 夹具工程
# （例如 p08_btcore_mockers3/r20_mq_fixture，自带 int main 与 fake MQ 导出），
# 由其自身工程单独构建。dir: lane 只做非递归 GLOB，这些文件不会被分派走，
# 若留在主目标会以自带 main 取代 gtest_main。这里从递归源列表整目录排除。
file(GLOB_RECURSE WT_COV_STANDALONE_PROJECTS CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
FOREACH(_proj ${WT_COV_STANDALONE_PROJECTS})
	GET_FILENAME_COMPONENT(_proj_dir "${_proj}" DIRECTORY)
	SET(_kept_srcs "")
	FOREACH(_s ${COV_SRCS})
		STRING(FIND "${_s}" "${_proj_dir}/" _pos)
		IF(NOT _pos EQUAL 0)
			LIST(APPEND _kept_srcs "${_s}")
		ENDIF()
	ENDFOREACH()
	SET(COV_SRCS ${_kept_srcs})
	MESSAGE(STATUS "coverage: 独立夹具工程 ${_proj_dir} 不并入 CoverageTests 主目标")
ENDFOREACH()

# ModuleHelper.hpp now uses a translation-unit-local anchor. Keep this workaround for
# legacy product TUs such as TraderDD.cpp and TraderHTS.cpp that define their own external
# inst_hlp() symbols. The dedicated BUG-L00-008 regression below bypasses this macro.
# Apply after other source COMPILE_DEFINITIONS so this mapping is not overwritten.
FUNCTION(WT_COV_ISOLATE_INST_HLP)
	FOREACH(_s ${ARGN})
		STRING(MD5 _h "${_s}")
		STRING(SUBSTRING "${_h}" 0 8 _h)
		GET_SOURCE_FILE_PROPERTY(_old "${_s}" COMPILE_DEFINITIONS)
		SET_SOURCE_FILES_PROPERTIES("${_s}" PROPERTIES
			COMPILE_DEFINITIONS "${_old};inst_hlp=cov_inst_hlp_${_h}")
	ENDFOREACH()
ENDFUNCTION()

WT_COV_ISOLATE_INST_HLP(${COV_SRCS})

# 只有 dylib（hidden 内部不外泄，不会与编进来的 TU 撞名）与静态的 WTSTools/WTSUtils 可以共用
SET(WT_COV_SHARED_LIBS
	GTest::gtest_main Threads::Threads
	WTSTools WTSUtils
	WtShareHelper WtDataStorage WtDataStorageAD WtIndexFactory
	WtDtHelper WtDtPorter WtDtServo WtBtPorter
	WtExeFact WtExecMon WtRiskMonFact
	WtCtaTwoMaFact WtCtaStraFact WtUftStraFact
	WtMsgQue WtPorter nanomsg ${ICONV_LIBRARY})
# 独立目标若不链任何静态核心，Boost 符号就没有出处，所以在此显式补齐
LIST(APPEND WT_COV_SHARED_LIBS boost_filesystem boost_thread)

# ---------------------------------------------------------------------------
# 本机有两套 boost：/usr/local/include 是 1.81，Homebrew(/opt/homebrew) 是 1.88，
# 而链接行走 -L/opt/homebrew/lib 优先 → 实际链的是 1.88。
# 生产目标的编译行是 -I/opt/homebrew/include（实测其目标文件要的是
# detail::path_algorithms::compare_v3 这类 1.88 形态符号）。
# 测试目标多了一处 -isystem /opt/homebrew/include：它来自被链接目标的
# INTERFACE_SYSTEM_INCLUDE_DIRECTORIES（Homebrew GTest config、以及导出 ${CMAKE_SOURCE_DIR}
# 的产品库）。clang 按路径去重，同名目录一旦以 -isystem 登记，再补 -I 也失效（实测：
# 只有 -I → 1_88；-I 与 -isystem 同时存在，无论前后 → 1_81），
# 而 clang 又把内置的 /usr/local/include 排在 -isystem 之前 →
# 于是测试 TU 用 1.81 头编、链 1.88 库，符号形不对（path::compare_v3/extension_v3/stem_v3、
# detail::directory_iterator_construct 全未定义，实测 CoverageTests_dtcore 链失败）。
# 解法不能用「再补一次 -I」，也不能靠摘 INTERFACE_SYSTEM 属性（那是产品库的接口属性，
# 改了会波及其他可执行目标的告警面）。把已验证版本的真实 Boost 头目录复制到 build：
# 先解析 Boost_INCLUDE_DIR/boost 的真实路径，避免复制 Homebrew include 下的入口链接。
# 副本以 -I 形态进搜索链，仍排在内置 /usr/local/include 之前；重复 configure 复用副本。
SET(WT_COV_BOOST_BRIDGE_PARENT "${CMAKE_CURRENT_BINARY_DIR}/cov_boost_include")
SET(WT_COV_BOOST_BRIDGE "${WT_COV_BOOST_BRIDGE_PARENT}/boost")
SET(WT_COV_BOOST_BRIDGE_STATE "${WT_COV_BOOST_BRIDGE_PARENT}/.wt_cov_boost_bridge")

FUNCTION(WT_COV_BOOST_ASSERT_NO_SYMLINKS root description)
    FILE(GLOB_RECURSE _entries LIST_DIRECTORIES TRUE "${root}/*")
    FOREACH(_entry ${_entries})
        IF(IS_SYMLINK "${_entry}")
            MESSAGE(FATAL_ERROR "coverage: Boost ${description} 含符号链接，拒绝生成不封闭的 bridge：${_entry}")
        ENDIF()
    ENDFOREACH()
ENDFUNCTION()

FUNCTION(WT_COV_BOOST_TREE_SHA256 root out_var)
    FILE(GLOB_RECURSE _files LIST_DIRECTORIES FALSE "${root}/*")
    LIST(SORT _files)
    SET(_manifest "")
    FOREACH(_file IN LISTS _files)
        FILE(RELATIVE_PATH _relative "${root}" "${_file}")
        FILE(SHA256 "${_file}" _file_hash)
        STRING(APPEND _manifest "${_relative}\n${_file_hash}\n")
    ENDFOREACH()
    STRING(SHA256 _tree_hash "${_manifest}")
    SET(${out_var} "${_tree_hash}" PARENT_SCOPE)
ENDFUNCTION()

IF(NOT Boost_INCLUDE_DIR OR NOT IS_DIRECTORY "${Boost_INCLUDE_DIR}/boost")
    MESSAGE(FATAL_ERROR "coverage: Boost_INCLUDE_DIR 缺少 boost 子目录，无法钉住版本")
ENDIF()
GET_FILENAME_COMPONENT(_WT_COV_BOOST_SOURCE "${Boost_INCLUDE_DIR}/boost" REALPATH)
IF(NOT IS_DIRECTORY "${_WT_COV_BOOST_SOURCE}" OR NOT EXISTS "${_WT_COV_BOOST_SOURCE}/version.hpp")
    MESSAGE(FATAL_ERROR "coverage: Boost 头目录真实路径无效：${_WT_COV_BOOST_SOURCE}")
ENDIF()
WT_COV_BOOST_ASSERT_NO_SYMLINKS("${_WT_COV_BOOST_SOURCE}" "source tree")
FILE(STRINGS "${_WT_COV_BOOST_SOURCE}/version.hpp" _boost_version_defines
    REGEX "^[ ]*#[ ]*define[ ]+BOOST_VERSION[ ]+[0-9]+$")
FILE(STRINGS "${_WT_COV_BOOST_SOURCE}/version.hpp" _boost_lib_version_defines
    REGEX "^[ ]*#[ ]*define[ ]+BOOST_LIB_VERSION")
LIST(LENGTH _boost_version_defines _boost_version_define_count)
LIST(LENGTH _boost_lib_version_defines _boost_lib_version_define_count)
IF(NOT _boost_version_define_count EQUAL 1 OR NOT _boost_lib_version_define_count EQUAL 1)
    MESSAGE(FATAL_ERROR "coverage: 无法从 Boost version.hpp 唯一解析版本")
ENDIF()
STRING(REGEX MATCH "BOOST_VERSION[ ]+([0-9]+)" _boost_version_match "${_boost_version_defines}")
SET(_boost_version_number "${CMAKE_MATCH_1}")
STRING(REGEX MATCH "[0-9]+_[0-9]+" _boost_lib_version "${_boost_lib_version_defines}")
IF(NOT _boost_version_number OR NOT _boost_lib_version)
    MESSAGE(FATAL_ERROR "coverage: Boost version.hpp 内容无效")
ENDIF()
MATH(EXPR _boost_major "${_boost_version_number} / 100000")
MATH(EXPR _boost_minor "(${_boost_version_number} / 100) % 1000")
MATH(EXPR _boost_patch "${_boost_version_number} % 100")
SET(_WT_COV_BOOST_VERSION "${_boost_major}.${_boost_minor}.${_boost_patch}")
STRING(REPLACE "." "_" _boost_lib_version_from_header "${_boost_major}.${_boost_minor}")
IF(NOT _boost_lib_version STREQUAL _boost_lib_version_from_header)
    MESSAGE(FATAL_ERROR "coverage: BOOST_LIB_VERSION 与 BOOST_VERSION 不一致")
ENDIF()

SET(_WT_COV_BOOST_PACKAGE_VERSION_FOUND FALSE)
IF(DEFINED Boost_VERSION_STRING AND NOT "${Boost_VERSION_STRING}" STREQUAL "")
    IF(NOT Boost_VERSION_STRING STREQUAL _WT_COV_BOOST_VERSION)
        MESSAGE(FATAL_ERROR "coverage: find_package(Boost) 版本 ${Boost_VERSION_STRING} 与头文件 ${_WT_COV_BOOST_VERSION} 不一致")
    ENDIF()
    SET(_WT_COV_BOOST_PACKAGE_VERSION_FOUND TRUE)
ENDIF()
IF(DEFINED Boost_VERSION AND NOT "${Boost_VERSION}" STREQUAL "")
    IF(Boost_VERSION MATCHES "^[0-9]+$")
        IF(NOT Boost_VERSION STREQUAL _boost_version_number)
            MESSAGE(FATAL_ERROR "coverage: find_package(Boost) 版本号 ${Boost_VERSION} 与头文件 ${_boost_version_number} 不一致")
        ENDIF()
    ELSEIF(NOT Boost_VERSION STREQUAL _WT_COV_BOOST_VERSION)
        MESSAGE(FATAL_ERROR "coverage: find_package(Boost) 版本 ${Boost_VERSION} 与头文件 ${_WT_COV_BOOST_VERSION} 不一致")
    ENDIF()
    SET(_WT_COV_BOOST_PACKAGE_VERSION_FOUND TRUE)
ENDIF()
IF(NOT _WT_COV_BOOST_PACKAGE_VERSION_FOUND)
    MESSAGE(FATAL_ERROR "coverage: find_package(Boost) 未提供可验证版本")
ENDIF()
IF(DEFINED Boost_LIB_VERSION AND NOT "${Boost_LIB_VERSION}" STREQUAL "" AND
    NOT Boost_LIB_VERSION STREQUAL _boost_lib_version)
    MESSAGE(FATAL_ERROR "coverage: find_package(Boost) 库版本 ${Boost_LIB_VERSION} 与头文件 ${_boost_lib_version} 不一致")
ENDIF()

WT_COV_BOOST_TREE_SHA256("${_WT_COV_BOOST_SOURCE}" _WT_COV_BOOST_SOURCE_TREE_HASH)
STRING(CONCAT _WT_COV_BOOST_EXPECTED_STATE
    "format=3\n"
    "source=${_WT_COV_BOOST_SOURCE}\n"
    "version=${_WT_COV_BOOST_VERSION}\n"
    "tree_sha256=${_WT_COV_BOOST_SOURCE_TREE_HASH}\n"
    "Boost_VERSION=${Boost_VERSION}\n"
    "Boost_VERSION_STRING=${Boost_VERSION_STRING}\n"
    "Boost_LIB_VERSION=${Boost_LIB_VERSION}\n"
    "Boost_DIR=${Boost_DIR}\n"
    "Boost_INCLUDE_DIR=${Boost_INCLUDE_DIR}\n"
    "Boost_LIBRARY_DIRS=${Boost_LIBRARY_DIRS}\n"
    "Boost_LIBRARIES=${Boost_LIBRARIES}\n")

IF(IS_SYMLINK "${WT_COV_BOOST_BRIDGE_PARENT}" OR
    (EXISTS "${WT_COV_BOOST_BRIDGE_PARENT}" AND NOT IS_DIRECTORY "${WT_COV_BOOST_BRIDGE_PARENT}"))
    MESSAGE(FATAL_ERROR "coverage: Boost bridge parent 是非预期实体，拒绝触碰：${WT_COV_BOOST_BRIDGE_PARENT}")
ENDIF()
FILE(MAKE_DIRECTORY "${WT_COV_BOOST_BRIDGE_PARENT}")
IF(IS_SYMLINK "${WT_COV_BOOST_BRIDGE_STATE}" OR IS_DIRECTORY "${WT_COV_BOOST_BRIDGE_STATE}")
    MESSAGE(FATAL_ERROR "coverage: Boost bridge state 是非预期实体，拒绝覆盖：${WT_COV_BOOST_BRIDGE_STATE}")
ENDIF()
SET(_WT_COV_BOOST_STATE_EXISTS FALSE)
IF(EXISTS "${WT_COV_BOOST_BRIDGE_STATE}")
    FILE(READ "${WT_COV_BOOST_BRIDGE_STATE}" _WT_COV_BOOST_STATE_CONTENT)
    IF(NOT _WT_COV_BOOST_STATE_CONTENT STREQUAL _WT_COV_BOOST_EXPECTED_STATE)
        MESSAGE(FATAL_ERROR "coverage: Boost find_package 来源或版本变化；拒绝覆盖现有 bridge")
    ENDIF()
    SET(_WT_COV_BOOST_STATE_EXISTS TRUE)
ENDIF()
SET(_WT_COV_BOOST_NEEDS_COPY TRUE)
IF(IS_SYMLINK "${WT_COV_BOOST_BRIDGE}")
    GET_FILENAME_COMPONENT(_WT_COV_BOOST_EXISTING_TARGET "${WT_COV_BOOST_BRIDGE}" REALPATH)
    IF(NOT IS_DIRECTORY "${WT_COV_BOOST_BRIDGE}" OR
        NOT _WT_COV_BOOST_EXISTING_TARGET STREQUAL _WT_COV_BOOST_SOURCE)
        MESSAGE(FATAL_ERROR "coverage: 现有 Boost bridge 链接不指向当前预期头目录，拒绝移除：${WT_COV_BOOST_BRIDGE}")
    ENDIF()
    FILE(REMOVE "${WT_COV_BOOST_BRIDGE}")
ELSEIF(EXISTS "${WT_COV_BOOST_BRIDGE}")
    IF(NOT IS_DIRECTORY "${WT_COV_BOOST_BRIDGE}")
        MESSAGE(FATAL_ERROR "coverage: 现有 Boost bridge 是非预期实体，拒绝覆盖：${WT_COV_BOOST_BRIDGE}")
    ENDIF()
    IF(NOT _WT_COV_BOOST_STATE_EXISTS)
        MESSAGE(FATAL_ERROR "coverage: 现有 Boost bridge 无生成标记，拒绝覆盖用户目录：${WT_COV_BOOST_BRIDGE}")
    ENDIF()
    SET(_WT_COV_BOOST_NEEDS_COPY FALSE)
ELSEIF(_WT_COV_BOOST_STATE_EXISTS)
    MESSAGE(FATAL_ERROR "coverage: Boost bridge 标记存在但目录缺失，拒绝自动重建")
ENDIF()
IF(_WT_COV_BOOST_NEEDS_COPY)
    FILE(COPY "${_WT_COV_BOOST_SOURCE}" DESTINATION "${WT_COV_BOOST_BRIDGE_PARENT}")
ENDIF()
IF(IS_SYMLINK "${WT_COV_BOOST_BRIDGE}" OR NOT IS_DIRECTORY "${WT_COV_BOOST_BRIDGE}")
    MESSAGE(FATAL_ERROR "coverage: Boost bridge 不是已生成的真实目录")
ENDIF()
WT_COV_BOOST_ASSERT_NO_SYMLINKS("${WT_COV_BOOST_BRIDGE}" "bridge copy")
WT_COV_BOOST_TREE_SHA256("${WT_COV_BOOST_BRIDGE}" _WT_COV_BOOST_BRIDGE_TREE_HASH)
IF(NOT _WT_COV_BOOST_SOURCE_TREE_HASH STREQUAL _WT_COV_BOOST_BRIDGE_TREE_HASH)
    MESSAGE(FATAL_ERROR "coverage: Boost bridge 与当前源头文件树摘要不一致；拒绝复用")
ENDIF()
IF(NOT _WT_COV_BOOST_STATE_EXISTS)
    FILE(WRITE "${WT_COV_BOOST_BRIDGE_STATE}" "${_WT_COV_BOOST_EXPECTED_STATE}")
ENDIF()

# 桥接目录放最前，保证 boost 头先以 -I 命中
SET(WT_COV_INCS
	"${CMAKE_CURRENT_BINARY_DIR}/cov_boost_include"
	${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/Includes ${CMAKE_SOURCE_DIR}/Share
	${CMAKE_SOURCE_DIR}/WTSTools ${CMAKE_SOURCE_DIR}/WTSUtils)

# 厂商头有二级 include（如 <mds_api/mds_api.h>、<trade/atp_trade_types.h>），
# 只挂 src/ 一个根不够 —— 必须把厂商包自己的 include 根也挂上。
# 目录名一律从模块源码的 `#include <API/…>` 里抽，不手敲（手敲会写错版本号目录，
# 而且换厂商版本时会静默失效）。挂成 SYSTEM，厂商头里的告警不该被我们的 -Werror 拦。
FUNCTION(WT_COV_VENDOR_INCS out_var)
	SET(_incs "")
	FOREACH(f ${ARGN})
		IF(NOT EXISTS "${f}")
			CONTINUE()
		ENDIF()
		file(STRINGS "${f}" _lines REGEX "#include[ ]*<API/")
		FOREACH(l ${_lines})
			STRING(REGEX MATCH "<API/([A-Za-z0-9._-]+)" _m "${l}")
			SET(_d "${CMAKE_SOURCE_DIR}/API/${CMAKE_MATCH_1}")
			IF(NOT IS_DIRECTORY "${_d}")
				MESSAGE(STATUS "coverage: 厂商头目录不存在，跳过：${_d}")
				CONTINUE()
			ENDIF()
			LIST(APPEND _incs "${_d}")
			FOREACH(sub include Include x64 include/x64)
				IF(IS_DIRECTORY "${_d}/${sub}")
					LIST(APPEND _incs "${_d}/${sub}")
				ENDIF()
			ENDFOREACH()
		ENDFOREACH()
	ENDFOREACH()
	LIST(REMOVE_DUPLICATES _incs)
	SET(${out_var} "${_incs}" PARENT_SCOPE)
ENDFUNCTION()

# 2026-10-28 owner 批准：「测试侧按 TU 单独放宽」——只对下面这些指定告警关掉 -Werror，
# 不改产品代码、不动全局门槛、不影响产品自己的构建。适用对象只有两类：
#   (a) 本机从来没被编译过的厂商适配层 TU（ParserYD/ParserHuaX/TraderATP/TraderXTPXAlgo/ParserOES），
#       它们的历史告警（-Wunused-parameter、-Wreorder-ctor）会以 -Werror 打死目标；
#   (b) 被 `-Dmain=<模块>_coverage_main` 改名的可执行体 TU —— clang 只豁免「真叫 main 的函数」
#       不写 return，改名后豁免消失（实测 WtBtRunner.cpp:160 -Werror,-Wreturn-type）。
# 逐处依据与行号见 docs/迭代计划/迭代01/覆盖排除清单.md 第 6 节 与 bugs/l00_build_graph.md BUG-L00-009。
SET(WT_COV_RELAXED_WARNINGS
	-Wno-error=unused-parameter
	-Wno-error=reorder-ctor
	-Wno-error=return-type
	-Wno-error=unused-variable)
FUNCTION(WT_COV_RELAX_WARNINGS)
	FOREACH(_s ${ARGN})
		GET_SOURCE_FILE_PROPERTY(_old "${_s}" COMPILE_OPTIONS)
		SET_SOURCE_FILES_PROPERTIES("${_s}" PROPERTIES
			COMPILE_OPTIONS "${_old};${WT_COV_RELAXED_WARNINGS}")
	ENDFOREACH()
ENDFUNCTION()

# ---------------------------------------------------------------------------
# 拆分表： <目标后缀>|<生产目录，逗号分隔>|<用例来源，逗号分隔>
# 用例来源以 "dir:" 开头 = 该目录下的全部 .cpp（新 lane 不必改表就能进目标）；
# 否则按相对路径逐文件登记（用于跨插件引用纠缠、暂时无法整目录归族的那些）。
# 分隔符刻意避开分号——分号是 CMake 的列表分隔符，set() 里的引号拦不住它（实测会把一条
# spec 提前拆开，报 LIST index out of range）。
SET(WT_COV_SPLIT_TARGETS
	"wtcore|WtCore|dir:n01_wtcore_tradepaths,dir:n02_wtcore_engines,dir:p01_wtcore_stractx,dir:p02_wtcore_engines2,dir:p11_wtcore_traderadapter,dir:q01_wtcore3"
	"uftcore|WtUftCore|dir:n03_uftcore_index,dir:p06_uftcore2,dir:q04_uftcore3"
	"btcore|WtBtCore|dir:l06_wtbtcore,dir:n04_btcore_mockers,dir:p03_btcore_mockers2,dir:p04_btcore_replayer,dir:p07_btcore_replayer2,dir:p08_btcore_mockers3,dir:q02_btcore3"
	# WtDtCore 是 STATIC 且与 WtCore 有同名 wtp::ParserAdapter/ParserAdapterMgr/WtHelper，
	# 两者一起链进主目标会被静态库惰性抽取静默择一（BUG-L00-005），所以单独立目标；
	# WtDtServo/WtDtHelper 是 hidden SHARED，其 TU 一并编进来才能测内部。
	# WtDtCore 与 WtDtServo 都有一个 class ParserAdapter，但两边形状不同
	# （WtDtCore/ParserAdapter.h:30 构造是 3 参，WtDtServo/ParserAdapter.h:28 是 2 参），
	# 同名不同形 → 不能塞进同一个二进制（不是去重能解决的），所以拆成两个目标。
	"dtcore|WtDtCore,WtDtHelper|dir:q05_dtsrv"
	"dtservo|WtDtServo|dir:q05b_dtservo"
	# 实盘/回测的 C 导出层（WtPorter / WtBtPorter）此前没有任何目标编过它们的 TU，
	# 两块合计 3390 行未覆盖。它们各自定义 extern "C" 入口，所以必须一人一个二进制。
	"porter|WtPorter|dir:q06_porter"
	"btporter|WtBtPorter|dir:q06_btporter"
	# WtDataStorage 与 WtDataStorageAD 各定义一套同名 extern "C" 工厂入口（实测
	# createWriter/createBtDtReader/createDataReader/createRdmDtReader 及对应 delete
	# 每个名字命中 2 处），两目录同进一个二进制必 duplicate，所以拆开：
	# WtDataStorage 的 TU 留在主目标（l04 用例用它），AD 单独成目标。
	"adstorage|WtDataStorageAD|dir:p05_datastorage2"
	"idxfactory|WtIndexFactory|n03b_idxfactory/n03_idxfactory_worker_test.cpp"
	"exefact|WtExeFact,WtExecMon|n05_exec_factories/n05_exe_fact_test.cpp+n05_exec_factories/n05_minimpact_calc_test.cpp+n05_exec_factories/n05_channel_ready_test.cpp+n05_exec_factories/n05_unmanaged_cancel_latch_test.cpp+n05_exec_factories/n05_ordmon_test.cpp+dir:r7_exefact+dir:r24_exefact2"
	"riskmon|WtRiskMonFact,WtExecMon|n05_exec_factories/n05_riskmon_datamgr_test.cpp"
	"mqshare|WtShareHelper,WtMsgQue|n06_strategies_mq_share/n06_mq_share_test.cpp+dir:r19_mq")

# ---------------------------------------------------------------------------
# 收发族（vendor adapter）：一个目录 = 一个目标
# 每个收发目录都各自定义 extern "C" 的 createParser/createTrader/deleteParser/
# deleteTrader（实测同名），所以两个目录进同一个二进制必然 duplicate symbol。
# 厂商头本身在仓库里（src/API/CTP6.6.9、Femas3.02 等），顶层构建也已经在编译它们，
# 所以「测不了」并不成立——缺的只是每族一个可执行文件与伪造回调的驱动。
# 用例目录约定：src/TestUnits/coverage/t_<模块名>/*.cpp，目录为空时该目标自动跳过。
# 模块清单从磁盘派生，不手敲（手敲会像前几轮一样写出仓库里不存在的名字）
FILE(GLOB WT_TRANSPORT_RAW RELATIVE "${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/Parser*" "${CMAKE_SOURCE_DIR}/Trader*" "${CMAKE_SOURCE_DIR}/*Loader")
# 通配会连 parsers.sln / traders.sln 这类文件一起捞进来，只保留真目录
set(WT_TRANSPORT_DIRS "")
FOREACH(cand ${WT_TRANSPORT_RAW})
	IF(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${cand}")
		LIST(APPEND WT_TRANSPORT_DIRS "${cand}")
	ENDIF()
ENDFOREACH()
FOREACH(mod ${WT_TRANSPORT_DIRS})
	LIST(APPEND WT_COV_SPLIT_TARGETS "t_${mod}|${mod}|dir:t_${mod}")
ENDFOREACH()
LIST(LENGTH WT_COV_SPLIT_TARGETS _ntargets)
MESSAGE(STATUS "coverage: 收发族目录 ${WT_TRANSPORT_DIRS}；目标总数 ${_ntargets}")

SET(MOVED_SRCS "")
FOREACH(spec ${WT_COV_SPLIT_TARGETS})
	STRING(REPLACE "|" ";" parts "${spec}")
	LIST(LENGTH parts nparts)
	IF(NOT nparts EQUAL 3)
		MESSAGE(FATAL_ERROR "coverage: bad spec '${spec}'（期望 3 段，实得 ${nparts}）")
	ENDIF()
	LIST(GET parts 0 suffix)
	LIST(GET parts 1 dirstr)
	LIST(GET parts 2 testsstr)
	STRING(REPLACE "," ";" dirs "${dirstr}")
	STRING(REPLACE "," ";" srcspecs "${testsstr}")
	# 多条用例文件之间用 + 分隔，必须再拆一次。实测漏掉这步时「a+b+c」被当成单一路径，
	# exeplugins / strafact 两个目标因此静默消失（不是报错，是目标没了）。
	STRING(REPLACE "+" ";" srcspecs "${srcspecs}")

	SET(tus "")
	FOREACH(d ${dirs})
		file(GLOB one CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${d}/*.cpp")
		LIST(APPEND tus ${one})
	ENDFOREACH()
	# 多个目录会复制同名 TU（WtHelper.cpp 实测有 4 份、WtStraFact 实现 2 份）。
	# 一律摘掉会把「唯一提供者」也删掉 → 未定义符号（dtsrv 就是一个例子：它不链
	# libWtCore.a，WtDtCore 自己那份 WtHelper.cpp 是唯一实现）。
	# 正确规则是按 basename 去重：保留第一个，摘掉其余副本。
	set(kept_tus "")
	set(seen_bases "")
	FOREACH(t ${tus})
		GET_FILENAME_COMPONENT(_base "${t}" NAME)
		LIST(FIND seen_bases "${_base}" _dup)
		IF(_dup GREATER -1)
			MESSAGE(STATUS "coverage: CoverageTests_${suffix} 跳过重复 TU ${t}（已保留 ${_base} 的首个实现）")
			CONTINUE()
		ENDIF()
		LIST(APPEND seen_bases "${_base}")
		LIST(APPEND kept_tus "${t}")
	ENDFOREACH()
	set(tus ${kept_tus})

	SET(srcs "")
	FOREACH(sp ${srcspecs})
		IF(sp MATCHES "^dir:")
			STRING(SUBSTRING "${sp}" 4 -1 ldir)
			file(GLOB onedir CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${ldir}/*.cpp")
			FOREACH(s ${onedir})
				LIST(APPEND srcs "${s}")
				LIST(APPEND MOVED_SRCS "${s}")
			ENDFOREACH()
			IF(NOT onedir)
				MESSAGE(STATUS "coverage: CoverageTests_${suffix} 的目录 ${ldir} 暂无用例（lane 未产出或被跳过）")
			ENDIF()
		ELSEIF(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${sp}")
			SET(onesrc "${CMAKE_CURRENT_SOURCE_DIR}/${sp}")
			LIST(APPEND srcs "${onesrc}")
			LIST(APPEND MOVED_SRCS "${onesrc}")
		ELSE()
			MESSAGE(STATUS "coverage: CoverageTests_${suffix} 跳过（用例尚未产出：${sp}）")
		ENDIF()
	ENDFOREACH()
	IF(NOT srcs)
		CONTINUE()
	ENDIF()

	WT_COV_ISOLATE_INST_HLP(${tus})
	ADD_EXECUTABLE(CoverageTests_${suffix} ${srcs} ${tus})
	TARGET_INCLUDE_DIRECTORIES(CoverageTests_${suffix} PRIVATE ${WT_COV_INCS})
	IF(suffix STREQUAL "dtservo")
		TARGET_COMPILE_DEFINITIONS(CoverageTests_${suffix} PRIVATE WT_P12_BAR_LIFETIME_TEST_HOOKS)
	ENDIF()
	IF(suffix STREQUAL "t_TraderDumper")
		# Only the isolated test executable gets a deterministic worker-wait handshake
		# and legacy sleep control; the production TraderDumper target has no test API.
		TARGET_COMPILE_DEFINITIONS(CoverageTests_${suffix} PRIVATE WT_DUMPER_TEST_HOOKS)
	ENDIF()
	# 厂商头二级 include（<mds_api/…>、<trade/…>）：从模块源码的 #include <API/…> 反查目录
	WT_COV_VENDOR_INCS(_vincs ${tus})
	IF(_vincs)
		TARGET_INCLUDE_DIRECTORIES(CoverageTests_${suffix} SYSTEM PRIVATE ${_vincs})
	ENDIF()
	# 本机没有 darwin 版厂商库的收发模块（判据：该模块自己的 CMakeLists 里不引用 darwin 路径）
	# 允许厂商符号留空 —— 测试只走本地可控路径，一旦真调未定义符号会运行期失败，
	# 所以这不会把「没测到」伪装成「测过了」。有 darwin 库的族（CTP 系）继续走下面的 .a 清单，
	# 保持链接期严格，符号缺失照样报错。
	SET(_no_darwin_lib FALSE)
	FOREACH(d ${dirs})
		IF(EXISTS "${CMAKE_SOURCE_DIR}/${d}/CMakeLists.txt")
			file(STRINGS "${CMAKE_SOURCE_DIR}/${d}/CMakeLists.txt" _dlines REGEX "darwin")
			IF(NOT _dlines)
				SET(_no_darwin_lib TRUE)
			ENDIF()
		ENDIF()
	ENDFOREACH()
	IF(APPLE AND _no_darwin_lib AND NOT WT_COV_VENDOR_LIBS)
		TARGET_LINK_OPTIONS(CoverageTests_${suffix} PRIVATE "-Wl,-undefined,dynamic_lookup")
		MESSAGE(STATUS "coverage: CoverageTests_${suffix} 无 darwin 厂商库，链接期容忍未定义厂商符号（用例不得调用真实 API）")
	ENDIF()
	IF(suffix STREQUAL "porter")
		# This target uses WtCore; WtBtPorter brings in the incompatible WtBtCore
		# EventNotifier implementation, so keep the rest of the shared test deps only.
		SET(WT_COV_PORTER_SHARED_LIBS ${WT_COV_SHARED_LIBS})
		LIST(REMOVE_ITEM WT_COV_PORTER_SHARED_LIBS WtBtPorter)
		TARGET_LINK_LIBRARIES(CoverageTests_${suffix} PRIVATE ${WT_COV_PORTER_SHARED_LIBS})
	ELSEIF(suffix STREQUAL "uftcore")
		# I03-NEW-055: WtExecMon/WtPorter (and the other porter DSOs) embed the
		# WtCore/WtBtCore/WtDtCore static libraries with default visibility, so
		# their same-named wtp:: inline members are exported as weak definitions
		# and dyld may bind this binary's WtUftCore inline members to them at run
		# time. Link only what the UFT core itself links (as m_WtUftRunner does).
		SET(WT_COV_UFTCORE_LIBS
			GTest::gtest_main Threads::Threads
			WTSTools WTSUtils boost_filesystem boost_thread ${ICONV_LIBRARY})
		IF(CMAKE_DL_LIBS)
			LIST(APPEND WT_COV_UFTCORE_LIBS ${CMAKE_DL_LIBS})
		ENDIF()
		TARGET_LINK_LIBRARIES(CoverageTests_${suffix} PRIVATE ${WT_COV_UFTCORE_LIBS})
	ELSE()
		TARGET_LINK_LIBRARIES(CoverageTests_${suffix} PRIVATE ${WT_COV_SHARED_LIBS})
	ENDIF()
	# 厂商静态库：不手敲清单，直接从各模块自己的 CMakeLists 里读 `API/...*.a` 引用。
	# 实测只有 CTPLoader/ParserCTP/TraderCTP 三个目录需要（链接期报
	# CThostFtdcTraderApi::CreateFtdcTraderApi / CThostFtdcMdApi::CreateFtdcMdApi 未定义），
	# 其余 15 个收发目录不引厂商 .a，也不缺符号。列表顺序沿用产品文件里的书写顺序，
	# 因为静态归档按序惰性抽取（api 库依赖 ssl/crypto/comunicationkeylib）。
	SET(WT_COV_VENDOR_LIBS "")
	FOREACH(d ${dirs})
		SET(_mcf "${CMAKE_SOURCE_DIR}/${d}/CMakeLists.txt")
		IF(NOT EXISTS "${_mcf}")
			CONTINUE()
		ENDIF()
		SET_PROPERTY(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mcf}")
		file(STRINGS "${_mcf}" _apilines REGEX "API/[^ \t\r\n]+\\.a")
		FOREACH(l ${_apilines})
			STRING(REGEX MATCHALL "API/[^ \t\r\n]+\\.a" _hits "${l}")
			FOREACH(h ${_hits})
				SET(_p "${CMAKE_SOURCE_DIR}/${h}")
					IF(NOT EXISTS "${_p}")
										# 只登记本机真的存在的归档：产品文件同一处常并列 WIN32/Linux 分支，那些在本机不存在，
										# 不能把配置弄死；真缺的库会在链接期以未定义符号响亮暴露。
										MESSAGE(STATUS "coverage: 跳过不存在厂商库 ${_p}")
										CONTINUE()
					ENDIF()
				LIST(FIND WT_COV_VENDOR_LIBS "${_p}" _seen)
				IF(_seen EQUAL -1)
					LIST(APPEND WT_COV_VENDOR_LIBS "${_p}")
				ENDIF()
			ENDFOREACH()
		ENDFOREACH()
	ENDFOREACH()
	IF(WT_COV_VENDOR_LIBS)
		TARGET_LINK_LIBRARIES(CoverageTests_${suffix} PRIVATE ${WT_COV_VENDOR_LIBS})
	ENDIF()
	# porter/btporter 的 TU 需要各自核心的静态库符号（WtEngine/CtaMocker 等），
	# 而核心型目标按上面规则不链兄弟核心以免静默择一——这里按后缀精确补自己那一个核心，
	# 补两个就会撞上同名 wtp:: 类（BUG-L00-005）。
	if(suffix STREQUAL "porter")
		TARGET_LINK_LIBRARIES(CoverageTests_porter PRIVATE WtCore)
	elseif(suffix STREQUAL "btporter")
		TARGET_LINK_LIBRARIES(CoverageTests_btporter PRIVATE WtBtCore)
	endif()
	SET_TARGET_PROPERTIES(CoverageTests_${suffix} PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
ENDFOREACH()

# P07 resolves WtDataStorage through WtHelper's real module loader. The test's
# __FILE__ path identifies the source tree, so inject the exact out-of-source
# artifact instead of guessing a build directory relative to the source root.
if(NOT TARGET CoverageTests_btcore OR NOT TARGET WtDataStorage)
	message(FATAL_ERROR "P07 tick loader tests require CoverageTests_btcore and WtDataStorage")
endif()
set(WT_COV_P07_FIXTURE_ROOT "${CMAKE_SOURCE_DIR}/.." CACHE PATH
	"Root containing dist/common inputs required by P07 tick loader tests")
target_compile_definitions(CoverageTests_btcore PRIVATE
	"WT_COV_P07_FIXTURE_ROOT=\"${WT_COV_P07_FIXTURE_ROOT}\""
	"WT_COV_P07_DATA_STORAGE_MODULE=\"$<TARGET_FILE:WtDataStorage>\"")
add_dependencies(CoverageTests_btcore WtDataStorage)

# Q06 verifies the backtest factor-feed export through the exact WtBtPorter DSO
# handle. This test-only entry starts loadAdjFactors on that DSO's own runner so
# the external loader can exercise the exported feed function end to end.
IF(TARGET CoverageTests_btporter AND TARGET WtBtPorter)
	TARGET_COMPILE_DEFINITIONS(WtBtPorter PRIVATE WT_BT_PORTER_CAPI_TEST_HOOKS)
	TARGET_COMPILE_DEFINITIONS(CoverageTests_btporter PRIVATE
		"WT_COV_BTPORTER_LIBRARY_PATH=\"$<TARGET_FILE:WtBtPorter>\"")
	ADD_DEPENDENCIES(CoverageTests_btporter WtBtPorter)
	IF(CMAKE_DL_LIBS)
		TARGET_LINK_LIBRARIES(CoverageTests_btporter PRIVATE ${CMAKE_DL_LIBS})
	ENDIF()
ENDIF()

# R205/R214 exercise WtBtPorter's real binary replay loader. Resolve the module
# through the exact build target so out-of-source coverage builds need no path guesses.
if(TARGET CoverageTests_btporter AND TARGET WtDataStorage)
	target_compile_definitions(CoverageTests_btporter PRIVATE
		"WT_COV_BTPORTER_DATA_STORAGE_MODULE=\"$<TARGET_FILE:WtDataStorage>\"")
	add_dependencies(CoverageTests_btporter WtDataStorage)
endif()

# Q05 covers WtDataManager's real dlopen path. Put the null-reader fixture beside
# the storage/index plugins, where WtHelper's module directory resolves names.
if(TARGET CoverageTests_dtservo)
	set(WT_COV_Q05_READER_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin")
	add_library(Q05NullReaderFixture MODULE
		"${CMAKE_SOURCE_DIR}/TestUnits/fixtures/q05_null_reader_fixture.cpp")
	target_include_directories(Q05NullReaderFixture PRIVATE "${CMAKE_SOURCE_DIR}")
	set_target_properties(Q05NullReaderFixture PROPERTIES
		OUTPUT_NAME "q05_null_reader"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_Q05_READER_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_Q05_READER_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	if(WIN32)
		set_target_properties(Q05NullReaderFixture PROPERTIES SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(Q05NullReaderFixture PROPERTIES SUFFIX ".dylib")
	else()
		set_target_properties(Q05NullReaderFixture PROPERTIES SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_dtservo Q05NullReaderFixture)
	target_compile_definitions(CoverageTests_dtservo PRIVATE
		"WT_Q05_READER_MODULE_DIR=\"$<TARGET_FILE_DIR:WtIndexFactory>/\""
		"WT_Q05_NULL_READER_MODULE=\"q05_null_reader\"")
endif()

# R18-002 exercises DataManager's real writer dlopen/dlsym/init/release path.
# Each behavior is a separate DSO so symbol omission is verified by the OS
# loader rather than by a test-side mock of DLLHelper.
if(TARGET CoverageTests_dtcore)
	set(WT_COV_R18_WRITER_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin")
	set(WT_COV_R18_WRITER_FIXTURE "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r18_writer_fixture.cpp")

	function(WT_COV_R18_WRITER_TARGET target output)
		add_library(${target} MODULE "${WT_COV_R18_WRITER_FIXTURE}")
		target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}")
		set_target_properties(${target} PROPERTIES
			OUTPUT_NAME "${output}"
			LIBRARY_OUTPUT_DIRECTORY "${WT_COV_R18_WRITER_DIR}"
			RUNTIME_OUTPUT_DIRECTORY "${WT_COV_R18_WRITER_DIR}"
			CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
		if(WIN32)
			set_target_properties(${target} PROPERTIES SUFFIX ".dll")
		elseif(APPLE)
			set_target_properties(${target} PROPERTIES SUFFIX ".dylib")
		else()
			set_target_properties(${target} PROPERTIES SUFFIX ".so")
		endif()
	endfunction()

	WT_COV_R18_WRITER_TARGET(R18WriterSuccessFixture "r18_writer_success")
	WT_COV_R18_WRITER_TARGET(R18WriterInitFalseFixture "r18_writer_init_false")
	target_compile_definitions(R18WriterInitFalseFixture PRIVATE WT_R18_WRITER_INIT_FALSE)
	WT_COV_R18_WRITER_TARGET(R18WriterNullFactoryFixture "r18_writer_null_factory")
	target_compile_definitions(R18WriterNullFactoryFixture PRIVATE WT_R18_WRITER_NULL_FACTORY)
	WT_COV_R18_WRITER_TARGET(R18WriterMissingCreateFixture "r18_writer_missing_create")
	target_compile_definitions(R18WriterMissingCreateFixture PRIVATE WT_R18_WRITER_OMIT_CREATE)
	WT_COV_R18_WRITER_TARGET(R18WriterMissingDeleteFixture "r18_writer_missing_delete")
	target_compile_definitions(R18WriterMissingDeleteFixture PRIVATE WT_R18_WRITER_OMIT_DELETE)

	add_dependencies(CoverageTests_dtcore
		R18WriterSuccessFixture R18WriterInitFalseFixture R18WriterNullFactoryFixture
		R18WriterMissingCreateFixture R18WriterMissingDeleteFixture)
	target_compile_definitions(CoverageTests_dtcore PRIVATE
		"WT_R18_WRITER_MODULE_DIR=\"$<TARGET_FILE_DIR:R18WriterSuccessFixture>/\""
		"WT_R18_WRITER_SUCCESS_MODULE=\"r18_writer_success\""
		"WT_R18_WRITER_INIT_FALSE_MODULE=\"r18_writer_init_false\""
		"WT_R18_WRITER_NULL_FACTORY_MODULE=\"r18_writer_null_factory\""
		"WT_R18_WRITER_MISSING_CREATE_MODULE=\"r18_writer_missing_create\""
		"WT_R18_WRITER_MISSING_DELETE_MODULE=\"r18_writer_missing_delete\"")
endif()

# R68 uses an isolated counting plugin to check that reloading the same module
# neither recreates the factory nor invalidates an already active worker.
if(UNIX AND TARGET CoverageTests_dtcore)
	add_subdirectory("${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r68_index_factory"
		"${CMAKE_CURRENT_BINARY_DIR}/r68_index_factory")
endif()

# R45 exercises WtDtServo's actual parser-module path with a deterministic fake
# IParserApi. Keep the module separate from the coverage executable so its
# createParser/deleteParser exports are only visible through dlopen/dlsym.
if(TARGET CoverageTests_dtservo)
	# The exported C API (::initialize) resolves parser modules from <executable dir>/parsers, so
	# the fixture lives beside the test executable; probe, runner and exported cases share one image.
	set(WT_COV_R45_DTSERVO_PARSER_ROOT "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits")
	add_library(R45DtServoParserFixture MODULE
		"${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r45_dtservo_parser_fixture.cpp")
	target_include_directories(R45DtServoParserFixture PRIVATE ${WT_COV_INCS})
	set_target_properties(R45DtServoParserFixture PROPERTIES
		OUTPUT_NAME "R45DtServoParserFixture"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_R45_DTSERVO_PARSER_ROOT}/parsers"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_R45_DTSERVO_PARSER_ROOT}/parsers"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	if(WIN32)
		set_target_properties(R45DtServoParserFixture PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(R45DtServoParserFixture PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(R45DtServoParserFixture PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_dtservo R45DtServoParserFixture)
	target_compile_definitions(CoverageTests_dtservo PRIVATE
		"WT_R45_DTSERVO_PARSER_FIXTURE_PATH=\"$<TARGET_FILE:R45DtServoParserFixture>\"")
endif()

# T2-OPT-01 exercises ParserCTPOpt's real dlopen/dlsym path. These controlled
# modules are placed beside the coverage executable because the parser resolves
# ctpmodule relative to getBinDir(); only the success/null modules export the
# vendor creator symbol.
if(TARGET CoverageTests_t_ParserCTPOpt)
	set(WT_COV_T2_OPT_LOADER_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits")
	set(WT_COV_T2_OPT_LOADER_FIXTURE "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/t2_opt01_loader_fixture.cpp")
	set(WT_COV_T2_OPT_MISSING_SYMBOL_FIXTURE "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/t2_opt01_missing_symbol_fixture.cpp")

	add_library(T2Opt01MissingSymbol MODULE "${WT_COV_T2_OPT_MISSING_SYMBOL_FIXTURE}")
	set_target_properties(T2Opt01MissingSymbol PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_r105_ctpopt_missing_symbol"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T2_OPT_LOADER_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T2_OPT_LOADER_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(T2Opt01NullCreator MODULE "${WT_COV_T2_OPT_LOADER_FIXTURE}")
	target_include_directories(T2Opt01NullCreator PRIVATE "${CMAKE_SOURCE_DIR}")
	target_compile_definitions(T2Opt01NullCreator PRIVATE WT_R105_CTP_OPT_NULL_CREATOR)
	set_target_properties(T2Opt01NullCreator PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_r105_ctpopt_null_creator"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T2_OPT_LOADER_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T2_OPT_LOADER_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(T2Opt01SuccessfulCreator MODULE "${WT_COV_T2_OPT_LOADER_FIXTURE}")
	target_include_directories(T2Opt01SuccessfulCreator PRIVATE "${CMAKE_SOURCE_DIR}")
	set_target_properties(T2Opt01SuccessfulCreator PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_r105_ctpopt_successful_creator"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T2_OPT_LOADER_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T2_OPT_LOADER_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	if(WIN32)
		target_compile_definitions(T2Opt01NullCreator PRIVATE ISLIB LIB_MD_API_EXPORT)
		target_compile_definitions(T2Opt01SuccessfulCreator PRIVATE ISLIB LIB_MD_API_EXPORT)
		set_target_properties(T2Opt01MissingSymbol T2Opt01NullCreator T2Opt01SuccessfulCreator PROPERTIES SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(T2Opt01MissingSymbol T2Opt01NullCreator T2Opt01SuccessfulCreator PROPERTIES SUFFIX ".dylib")
	else()
		set_target_properties(T2Opt01MissingSymbol T2Opt01NullCreator T2Opt01SuccessfulCreator PROPERTIES SUFFIX ".so")
	endif()

	add_dependencies(CoverageTests_t_ParserCTPOpt
		T2Opt01MissingSymbol T2Opt01NullCreator T2Opt01SuccessfulCreator)
	target_compile_definitions(CoverageTests_t_ParserCTPOpt PRIVATE
		"WT_R105_CTP_OPT_MISSING_SYMBOL_MODULE=\"wt_r105_ctpopt_missing_symbol\""
		"WT_R105_CTP_OPT_NULL_CREATOR_MODULE=\"wt_r105_ctpopt_null_creator\""
		"WT_R105_CTP_OPT_SUCCESSFUL_CREATOR_MODULE=\"wt_r105_ctpopt_successful_creator\"")
endif()

# T1-XTP-01 exercises ParserXTP's real dlopen/dlsym path with local modules.
# The loader resolves modules beside the coverage executable; the test target
# therefore depends on these fixtures being placed in that same directory.
if(TARGET CoverageTests_t_ParserXTP)
	set(WT_COV_T1_XTP_LOADER_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits")
	set(WT_COV_T1_XTP_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/t1_xtp")
	set(WT_COV_T1_XTP_UNIX_PREFIX "lib")
	if(WIN32)
		set(WT_COV_T1_XTP_UNIX_PREFIX "")
	endif()

	add_library(T1XtpLoaderMissingSymbol MODULE "${WT_COV_T1_XTP_FIXTURE_DIR}/missing_symbol.cpp")
	set_target_properties(T1XtpLoaderMissingSymbol PROPERTIES
		PREFIX "" OUTPUT_NAME "${WT_COV_T1_XTP_UNIX_PREFIX}t1_xtp_missing_symbol"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T1_XTP_LOADER_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T1_XTP_LOADER_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(T1XtpLoaderNullCreator MODULE "${WT_COV_T1_XTP_FIXTURE_DIR}/creator_fixture.cpp")
	target_include_directories(T1XtpLoaderNullCreator PRIVATE "${CMAKE_SOURCE_DIR}")
	target_compile_definitions(T1XtpLoaderNullCreator PRIVATE WT_T1_XTP_NULL_CREATOR)
	set_target_properties(T1XtpLoaderNullCreator PROPERTIES
		PREFIX "" OUTPUT_NAME "${WT_COV_T1_XTP_UNIX_PREFIX}t1_xtp_null_creator"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T1_XTP_LOADER_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T1_XTP_LOADER_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(T1XtpLoaderSuccessfulCreator MODULE "${WT_COV_T1_XTP_FIXTURE_DIR}/creator_fixture.cpp")
	target_include_directories(T1XtpLoaderSuccessfulCreator PRIVATE "${CMAKE_SOURCE_DIR}")
	set_target_properties(T1XtpLoaderSuccessfulCreator PROPERTIES
		PREFIX "" OUTPUT_NAME "${WT_COV_T1_XTP_UNIX_PREFIX}t1_xtp_successful_creator"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T1_XTP_LOADER_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T1_XTP_LOADER_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	if(WIN32)
		target_compile_definitions(T1XtpLoaderNullCreator PRIVATE ISLIB LIB_MD_API_EXPORT)
		target_compile_definitions(T1XtpLoaderSuccessfulCreator PRIVATE ISLIB LIB_MD_API_EXPORT)
		set_target_properties(T1XtpLoaderMissingSymbol T1XtpLoaderNullCreator T1XtpLoaderSuccessfulCreator PROPERTIES SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(T1XtpLoaderMissingSymbol T1XtpLoaderNullCreator T1XtpLoaderSuccessfulCreator PROPERTIES SUFFIX ".dylib")
	else()
		set_target_properties(T1XtpLoaderMissingSymbol T1XtpLoaderNullCreator T1XtpLoaderSuccessfulCreator PROPERTIES SUFFIX ".so")
	endif()

	add_dependencies(CoverageTests_t_ParserXTP
		T1XtpLoaderMissingSymbol T1XtpLoaderNullCreator T1XtpLoaderSuccessfulCreator)
	target_compile_definitions(CoverageTests_t_ParserXTP PRIVATE
		"WT_T1_XTP_MISSING_SYMBOL_MODULE=\"t1_xtp_missing_symbol\""
		"WT_T1_XTP_NULL_CREATOR_MODULE=\"t1_xtp_null_creator\""
		"WT_T1_XTP_SUCCESSFUL_CREATOR_MODULE=\"t1_xtp_successful_creator\"")
endif()

# T4-007 exercises TraderFemas' real dlopen/dlsym path. Keep controlled modules
# next to its coverage executable, which resolves ctpmodule relative to getBinDir().
if(TARGET CoverageTests_t_TraderFemas)
	set(WT_COV_T4_FEMAS_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/t4_femas")
	set(WT_COV_T4_FEMAS_TEST_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits")

	add_library(T4FemasMissingSymbolFixture MODULE "${WT_COV_T4_FEMAS_FIXTURE_DIR}/missing_symbol.cpp")
	set_target_properties(T4FemasMissingSymbolFixture PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t4_femas_missing_symbol"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T4_FEMAS_TEST_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T4_FEMAS_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(T4FemasNullCreatorFixture MODULE "${WT_COV_T4_FEMAS_FIXTURE_DIR}/creator_fixture.cpp")
	target_include_directories(T4FemasNullCreatorFixture PRIVATE ${WT_COV_INCS} "${CMAKE_CURRENT_SOURCE_DIR}/t_TraderFemas")
	target_compile_definitions(T4FemasNullCreatorFixture PRIVATE WT_T4_FEMAS_NULL_CREATOR)
	set_target_properties(T4FemasNullCreatorFixture PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t4_femas_null_creator"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T4_FEMAS_TEST_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T4_FEMAS_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(T4FemasSuccessfulCreatorFixture MODULE "${WT_COV_T4_FEMAS_FIXTURE_DIR}/creator_fixture.cpp")
	target_include_directories(T4FemasSuccessfulCreatorFixture PRIVATE ${WT_COV_INCS} "${CMAKE_CURRENT_SOURCE_DIR}/t_TraderFemas")
	target_compile_definitions(T4FemasSuccessfulCreatorFixture PRIVATE WT_T4_FEMAS_SUCCESSFUL_CREATOR)
	set_target_properties(T4FemasSuccessfulCreatorFixture PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t4_femas_successful_creator"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T4_FEMAS_TEST_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T4_FEMAS_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	if(WIN32)
		target_compile_definitions(T4FemasNullCreatorFixture PRIVATE LIB_TRADER_API_EXPORT)
		target_compile_definitions(T4FemasSuccessfulCreatorFixture PRIVATE LIB_TRADER_API_EXPORT)
		set_target_properties(T4FemasMissingSymbolFixture T4FemasNullCreatorFixture T4FemasSuccessfulCreatorFixture PROPERTIES SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(T4FemasMissingSymbolFixture T4FemasNullCreatorFixture T4FemasSuccessfulCreatorFixture PROPERTIES SUFFIX ".dylib")
	else()
		set_target_properties(T4FemasMissingSymbolFixture T4FemasNullCreatorFixture T4FemasSuccessfulCreatorFixture PROPERTIES SUFFIX ".so")
	endif()

	add_dependencies(CoverageTests_t_TraderFemas
		T4FemasMissingSymbolFixture T4FemasNullCreatorFixture T4FemasSuccessfulCreatorFixture)
	target_compile_definitions(CoverageTests_t_TraderFemas PRIVATE
		"WT_T4_FEMAS_MISSING_SYMBOL_MODULE=\"wt_t4_femas_missing_symbol\""
		"WT_T4_FEMAS_NULL_CREATOR_MODULE=\"wt_t4_femas_null_creator\""
		"WT_T4_FEMAS_SUCCESSFUL_CREATOR_MODULE=\"wt_t4_femas_successful_creator\""
		"WT_T4_FEMAS_MISSING_LIBRARY_MODULE=\"wt_t4_femas_module_absent\"")
endif()

# T4-014 exercises CTPOptLoader's complete-config dlopen/dlsym/factory path with
# isolated fake modules. Each test copies its selected module into a temporary
# working directory, so no vendor library or flow output can escape the fixture.
if(TARGET CoverageTests_t_CTPOptLoader)
	set(WT_COV_T4_CTP_OPT_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/t4_ctpopt_loader")
	set(WT_COV_T4_CTP_OPT_TEST_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits")

	add_library(T4CtpOptMissingSymbolFixture MODULE "${WT_COV_T4_CTP_OPT_FIXTURE_DIR}/missing_symbol.cpp")
	set_target_properties(T4CtpOptMissingSymbolFixture PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t4_ctpopt_missing_symbol"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T4_CTP_OPT_TEST_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T4_CTP_OPT_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	target_compile_definitions(T4CtpOptMissingSymbolFixture PRIVATE
		"WT_T4_CTP_OPT_UNLOAD_MARKER=\"ctpopt_missing_symbol_unloaded\"")

	add_library(T4CtpOptNullCreatorFixture MODULE "${WT_COV_T4_CTP_OPT_FIXTURE_DIR}/creator_fixture.cpp")
	target_include_directories(T4CtpOptNullCreatorFixture PRIVATE
		${WT_COV_INCS} "${CMAKE_CURRENT_SOURCE_DIR}/t_CTPOptLoader")
	target_compile_definitions(T4CtpOptNullCreatorFixture PRIVATE
		WT_T4_CTP_OPT_NULL_CREATOR
		"WT_T4_CTP_OPT_UNLOAD_MARKER=\"ctpopt_null_creator_unloaded\"")
	set_target_properties(T4CtpOptNullCreatorFixture PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t4_ctpopt_null_creator"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T4_CTP_OPT_TEST_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T4_CTP_OPT_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(T4CtpOptSuccessfulCreatorFixture MODULE "${WT_COV_T4_CTP_OPT_FIXTURE_DIR}/creator_fixture.cpp")
	target_include_directories(T4CtpOptSuccessfulCreatorFixture PRIVATE
		${WT_COV_INCS} "${CMAKE_CURRENT_SOURCE_DIR}/t_CTPOptLoader")
	set_target_properties(T4CtpOptSuccessfulCreatorFixture PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t4_ctpopt_successful_creator"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T4_CTP_OPT_TEST_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T4_CTP_OPT_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	if(WIN32)
		set_target_properties(T4CtpOptMissingSymbolFixture T4CtpOptNullCreatorFixture T4CtpOptSuccessfulCreatorFixture PROPERTIES SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(T4CtpOptMissingSymbolFixture T4CtpOptNullCreatorFixture T4CtpOptSuccessfulCreatorFixture PROPERTIES SUFFIX ".dylib")
	else()
		set_target_properties(T4CtpOptMissingSymbolFixture T4CtpOptNullCreatorFixture T4CtpOptSuccessfulCreatorFixture PROPERTIES SUFFIX ".so")
	endif()

	add_dependencies(CoverageTests_t_CTPOptLoader
		T4CtpOptMissingSymbolFixture T4CtpOptNullCreatorFixture T4CtpOptSuccessfulCreatorFixture)
	target_compile_definitions(CoverageTests_t_CTPOptLoader PRIVATE
		"WT_T4_CTP_OPT_MISSING_SYMBOL_FIXTURE=\"$<TARGET_FILE:T4CtpOptMissingSymbolFixture>\""
		"WT_T4_CTP_OPT_NULL_CREATOR_FIXTURE=\"$<TARGET_FILE:T4CtpOptNullCreatorFixture>\""
		"WT_T4_CTP_OPT_SUCCESSFUL_CREATOR_FIXTURE=\"$<TARGET_FILE:T4CtpOptSuccessfulCreatorFixture>\"")
endif()

# T3-006 exercises TraderCTPMini's real dlopen/dlsym path with controlled
# creator fixtures located beside CoverageTests_t_TraderCTPMini, as required by
# getBinDir(). Keep these fixtures outside coverage/ so their factory symbols
# are not linked into another coverage executable.
if(TARGET CoverageTests_t_TraderCTPMini)
	set(WT_T3_006_CTPMINI_FIXTURE "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/t3_006_ctpmini_api.cpp")
	set(WT_T3_006_CTPMINI_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits")
	set(WT_T3_006_CTPMINI_TEST_INCS
		"${CMAKE_SOURCE_DIR}"
		"${CMAKE_CURRENT_SOURCE_DIR}/t_TraderCTPMini")

	add_library(T3_006MiniMissingSymbol MODULE "${WT_T3_006_CTPMINI_FIXTURE}")
	target_include_directories(T3_006MiniMissingSymbol PRIVATE ${WT_T3_006_CTPMINI_TEST_INCS})
	target_compile_definitions(T3_006MiniMissingSymbol PRIVATE WT_T3_006_CTPMINI_MISSING_SYMBOL)
	set_target_properties(T3_006MiniMissingSymbol PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t3_006_ctpmini_missing_symbol"
		LIBRARY_OUTPUT_DIRECTORY "${WT_T3_006_CTPMINI_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_T3_006_CTPMINI_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(T3_006MiniNullCreator MODULE "${WT_T3_006_CTPMINI_FIXTURE}")
	target_include_directories(T3_006MiniNullCreator PRIVATE ${WT_T3_006_CTPMINI_TEST_INCS})
	target_compile_definitions(T3_006MiniNullCreator PRIVATE WT_T3_006_CTPMINI_NULL_CREATOR)
	set_target_properties(T3_006MiniNullCreator PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t3_006_ctpmini_null_creator"
		LIBRARY_OUTPUT_DIRECTORY "${WT_T3_006_CTPMINI_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_T3_006_CTPMINI_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(T3_006MiniSuccessfulCreator MODULE "${WT_T3_006_CTPMINI_FIXTURE}")
	target_include_directories(T3_006MiniSuccessfulCreator PRIVATE ${WT_T3_006_CTPMINI_TEST_INCS})
	set_target_properties(T3_006MiniSuccessfulCreator PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t3_006_ctpmini_success"
		LIBRARY_OUTPUT_DIRECTORY "${WT_T3_006_CTPMINI_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_T3_006_CTPMINI_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	if(WIN32)
		target_compile_definitions(T3_006MiniNullCreator PRIVATE ISLIB LIB_MD_API_EXPORT)
		target_compile_definitions(T3_006MiniSuccessfulCreator PRIVATE ISLIB LIB_MD_API_EXPORT)
		set_target_properties(T3_006MiniMissingSymbol T3_006MiniNullCreator T3_006MiniSuccessfulCreator PROPERTIES SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(T3_006MiniMissingSymbol T3_006MiniNullCreator T3_006MiniSuccessfulCreator PROPERTIES SUFFIX ".dylib")
	else()
		set_target_properties(T3_006MiniMissingSymbol T3_006MiniNullCreator T3_006MiniSuccessfulCreator PROPERTIES SUFFIX ".so")
	endif()

	add_dependencies(CoverageTests_t_TraderCTPMini
		T3_006MiniMissingSymbol T3_006MiniNullCreator T3_006MiniSuccessfulCreator)
	target_compile_definitions(CoverageTests_t_TraderCTPMini PRIVATE
		"WT_T3_006_CTPMINI_MISSING_SYMBOL_MODULE=\"wt_t3_006_ctpmini_missing_symbol\""
		"WT_T3_006_CTPMINI_NULL_CREATOR_MODULE=\"wt_t3_006_ctpmini_null_creator\""
		"WT_T3_006_CTPMINI_SUCCESS_MODULE=\"wt_t3_006_ctpmini_success\"")
endif()

# UFT TraderAdapter lifecycle tests use a real loadable fixture so factory,
# init, release and delete exceptions cross the same module boundary as plugins.
if(TARGET CoverageTests_uftcore)
	add_library(R78NoParserFixture MODULE "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r78_no_parser/no_parser.cpp")
	set_target_properties(R78NoParserFixture PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	if(WIN32)
		set_target_properties(R78NoParserFixture PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(R78NoParserFixture PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(R78NoParserFixture PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_uftcore R78NoParserFixture)
	target_compile_definitions(CoverageTests_uftcore PRIVATE
		"WT_R78_NO_PARSER_FIXTURE_PATH=\"$<TARGET_FILE:R78NoParserFixture>\"")
	add_library(UftLifecycleFakeTrader MODULE q04_uftcore3/fake_plugin/uft_lifecycle_trader.cpp)
	target_include_directories(UftLifecycleFakeTrader PRIVATE "${CMAKE_SOURCE_DIR}/src")
	set_target_properties(UftLifecycleFakeTrader PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/uft_lifecycle_plugins"
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/uft_lifecycle_plugins")
	if(WIN32)
		set_target_properties(UftLifecycleFakeTrader PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(UftLifecycleFakeTrader PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(UftLifecycleFakeTrader PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_uftcore UftLifecycleFakeTrader)
	target_compile_definitions(CoverageTests_uftcore PRIVATE
		"WT_UFT_LIFECYCLE_FAKE_PLUGIN_PATH=\"$<TARGET_FILE:UftLifecycleFakeTrader>\"")
endif()

# R59 validates TraderAdapter credentials through a successfully initialized
# API module. Keep the fixture outside coverage/ so its factory exports do not
# also get linked into CoverageTests_wtcore.
if(TARGET CoverageTests_wtcore)
	# R160 checks the exact MQ C ABI through a deterministic loadable module.
	# Keep the fake in its own directory so the existing inproc integration test
	# still loads the production WtMsgQue beside the test binary.
	add_library(R160FakeWtMsgQue MODULE "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r160_fake_wtmsgque.cpp")
	target_include_directories(R160FakeWtMsgQue PRIVATE "${CMAKE_SOURCE_DIR}")
	set_target_properties(R160FakeWtMsgQue PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
		OUTPUT_NAME "WtMsgQue"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r160_fake_mq"
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r160_fake_mq")
	if(WIN32)
		set_target_properties(R160FakeWtMsgQue PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(R160FakeWtMsgQue PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(R160FakeWtMsgQue PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_wtcore R160FakeWtMsgQue)
	target_compile_definitions(CoverageTests_wtcore PRIVATE
		"WT_R160_FAKE_MQ_DIR=\"$<TARGET_FILE_DIR:R160FakeWtMsgQue>\"")
	if(TARGET CoverageTests_uftcore)
		# Reuse the ABI fixture to observe the UFT notifier's create/destroy calls and confirm argument.
		add_dependencies(CoverageTests_uftcore R160FakeWtMsgQue)
		target_compile_definitions(CoverageTests_uftcore PRIVATE
			"WT_UFT_R160_FAKE_MQ_DIR=\"$<TARGET_FILE_DIR:R160FakeWtMsgQue>\"")
	endif()

	# R10 checks fail-closed handling when the required MQ log callback export is absent.
	add_library(R10MissingRegisterWtMsgQue MODULE
		"${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r10_missing_register_wtmsgque.cpp")
	target_include_directories(R10MissingRegisterWtMsgQue PRIVATE "${CMAKE_SOURCE_DIR}")
	set_target_properties(R10MissingRegisterWtMsgQue PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
		OUTPUT_NAME "WtMsgQue"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r10_missing_register_mq"
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r10_missing_register_mq")
	if(WIN32)
		set_target_properties(R10MissingRegisterWtMsgQue PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(R10MissingRegisterWtMsgQue PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(R10MissingRegisterWtMsgQue PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_wtcore R10MissingRegisterWtMsgQue)
	target_compile_definitions(CoverageTests_wtcore PRIVATE
		"WT_R10_MISSING_REGISTER_MQ_DIR=\"$<TARGET_FILE_DIR:R10MissingRegisterWtMsgQue>\"")

	# R10 also verifies the required create_server export is checked before any
	# plugin callback or worker is started. Keep this controlled module separate
	# from the missing-register module so each fixture omits exactly one export.
	add_library(R10MissingCreateWtMsgQue MODULE
		"${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r10_missing_create_wtmsgque.cpp")
	target_include_directories(R10MissingCreateWtMsgQue PRIVATE "${CMAKE_SOURCE_DIR}")
	set_target_properties(R10MissingCreateWtMsgQue PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
		OUTPUT_NAME "WtMsgQue"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r10_missing_create_mq"
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r10_missing_create_mq")
	if(WIN32)
		set_target_properties(R10MissingCreateWtMsgQue PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(R10MissingCreateWtMsgQue PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(R10MissingCreateWtMsgQue PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_wtcore R10MissingCreateWtMsgQue)
	target_compile_definitions(CoverageTests_wtcore PRIVATE
		"WT_R10_MISSING_CREATE_MQ_DIR=\"$<TARGET_FILE_DIR:R10MissingCreateWtMsgQue>\"")

	# R6 verifies that plugin creator exceptions stop at the exported strategy-factory C API.
	add_library(R6ThrowingStrategyFactory MODULE
		"${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r6_throwing_strategy_factory.cpp")
	set_target_properties(R6ThrowingStrategyFactory PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
		OUTPUT_NAME "r6_throwing_strategy_factory"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r6_throwing_factory"
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r6_throwing_factory")
	if(WIN32)
		set_target_properties(R6ThrowingStrategyFactory PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(R6ThrowingStrategyFactory PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(R6ThrowingStrategyFactory PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_porter R6ThrowingStrategyFactory)
	target_compile_definitions(CoverageTests_porter PRIVATE
		"WT_R6_THROWING_FACTORY_DIR=\"$<TARGET_FILE_DIR:R6ThrowingStrategyFactory>\"")

	add_library(R59CredentialsTrader MODULE "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r59_credentials_trader.cpp")
	target_include_directories(R59CredentialsTrader PRIVATE "${CMAKE_SOURCE_DIR}")
	set_target_properties(R59CredentialsTrader PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r59_credentials_plugins"
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r59_credentials_plugins")
	if(WIN32)
		set_target_properties(R59CredentialsTrader PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(R59CredentialsTrader PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(R59CredentialsTrader PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_wtcore R59CredentialsTrader)
	target_compile_definitions(CoverageTests_wtcore PRIVATE
		"R59_CREDENTIALS_PLUGIN_PATH=\"$<TARGET_FILE:R59CredentialsTrader>\"")

	# P11 risk tests use the real TraderMocker API with a temporary install dir.
	add_dependencies(CoverageTests_wtcore TraderMocker)
	target_compile_definitions(CoverageTests_wtcore PRIVATE
		"WT_P11_TRADERMOCKER_PATH=\"$<TARGET_FILE:TraderMocker>\"")

	# R81 risk rejection tests need an init()-based local trader plugin and
	# observable insert/cancel counts without starting a real or simulated session.
	add_library(R81RiskFakeTrader MODULE
		"${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r81_risk_fake_trader.cpp")
	target_include_directories(R81RiskFakeTrader PRIVATE "${CMAKE_SOURCE_DIR}")
	set_target_properties(R81RiskFakeTrader PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
		OUTPUT_NAME "R81RiskFakeTrader"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r81_risk_plugins"
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r81_risk_plugins")
	if(WIN32)
		set_target_properties(R81RiskFakeTrader PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(R81RiskFakeTrader PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(R81RiskFakeTrader PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_wtcore R81RiskFakeTrader)
	target_compile_definitions(CoverageTests_wtcore PRIVATE
		"WT_R81_RISK_FAKE_TRADER_PATH=\"$<TARGET_FILE:R81RiskFakeTrader>\"")

	# R130 verifies WtCore ParserAdapter config ownership through its real
	# loadable-module path with a deterministic, non-threaded parser API.
	add_library(R130ParserLifecycle MODULE
		"${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r130_parser_lifecycle.cpp")
	target_include_directories(R130ParserLifecycle PRIVATE "${CMAKE_SOURCE_DIR}")
	set_target_properties(R130ParserLifecycle PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
		OUTPUT_NAME "R130LifecycleParser"
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r130_parser_plugins"
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r130_parser_plugins")
	if(WIN32)
		set_target_properties(R130ParserLifecycle PROPERTIES PREFIX "" SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(R130ParserLifecycle PROPERTIES PREFIX "lib" SUFFIX ".dylib")
	else()
		set_target_properties(R130ParserLifecycle PROPERTIES PREFIX "lib" SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_wtcore R130ParserLifecycle)
	target_compile_definitions(CoverageTests_wtcore PRIVATE
		"WT_R130_PARSER_PLUGIN_PATH=\"$<TARGET_FILE:R130ParserLifecycle>\"")
endif()

# Versioned execution-plugin loading contract: each fixture is a real loadable
# module so the host can be tested before and after dlclose boundaries.
set(WT_COV_EXEC_V2_PLUGIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/exec_v2_plugins")
file(MAKE_DIRECTORY "${WT_COV_EXEC_V2_PLUGIN_DIR}")
if(APPLE)
	# WtExecuterFactory scans .dylib on macOS (rather than CMake MODULE's .so).
	set(WT_COV_EXEC_V2_PLUGIN_SUFFIX ".dylib")
else()
	set(WT_COV_EXEC_V2_PLUGIN_SUFFIX "${CMAKE_SHARED_MODULE_SUFFIX}")
endif()
function(WT_COV_ADD_EXEC_V2_PLUGIN target output mode)
	add_library(${target} MODULE v2_exec_plugin/fake_exec_plugin.cpp)
	target_include_directories(${target} PRIVATE ${WT_COV_INCS})
	target_compile_definitions(${target} PRIVATE COV_EXEC_PLUGIN_MODE=${mode})
	set_target_properties(${target} PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
		PREFIX ""
		SUFFIX "${WT_COV_EXEC_V2_PLUGIN_SUFFIX}"
		OUTPUT_NAME "${output}"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_EXEC_V2_PLUGIN_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_EXEC_V2_PLUGIN_DIR}")
endfunction()
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2BadVersion bad_version 2)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2BadSize bad_size 3)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2InitFail init_fail 4)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2BadCallback bad_callback 5)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2Abi20 abi20 6)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2NoQuiesceCapability no_quiesce_capability 7)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2NoWaitQuiesce no_wait_quiesce 8)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2NoCanWaitQuiesce no_can_wait_quiesce 9)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2ShortQuiesce short_quiesce 10)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2NoRequestQuiesce no_request_quiesce 11)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2CanWaitFalse can_wait_false 12)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2CanWaitInvalid can_wait_invalid 13)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2RequestFail request_fail 14)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2WaitFail wait_fail 15)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2RequestThrow request_throw 16)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2CanWaitThrow can_wait_throw 17)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2WaitThrow wait_throw 18)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2InitRace init_race 19)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2Abi21 abi21 20)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2ShortFactoryPreflight short_factory_preflight 21)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2NoFactoryPreflightCapability no_factory_preflight_capability 22)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2NoFactoryPreflight no_factory_preflight 23)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2FactoryPreflightFalse factory_preflight_false 24)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2FactoryPreflightInvalid factory_preflight_invalid 25)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2FactoryPreflightThrow factory_preflight_throw 26)
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2NullFactory null_factory 27)
# BUG-N05-003 / K41 positive control: declares WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY.
WT_COV_ADD_EXEC_V2_PLUGIN(CovExecV2HostIdentity host_identity 28)

add_library(CovExecV2InitFailDrainsWorker MODULE
	v2_exec_plugin/init_fail_drains_worker_fixture.cpp)
target_include_directories(CovExecV2InitFailDrainsWorker PRIVATE ${WT_COV_INCS})
set_target_properties(CovExecV2InitFailDrainsWorker PROPERTIES
	CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
	PREFIX ""
	SUFFIX "${WT_COV_EXEC_V2_PLUGIN_SUFFIX}"
	OUTPUT_NAME "init_fail_drains_worker"
	LIBRARY_OUTPUT_DIRECTORY "${WT_COV_EXEC_V2_PLUGIN_DIR}"
	RUNTIME_OUTPUT_DIRECTORY "${WT_COV_EXEC_V2_PLUGIN_DIR}")

# WtBtRunner shutdown tests load these fixtures by filename at runtime.
if(TARGET CoverageTests_btporter)
	add_dependencies(CoverageTests_btporter CovExecV2FactoryPreflightFalse CovExecV2InitFail)
endif()

add_library(CovExecV2Valid MODULE v2_exec_plugin/fake_exec_plugin.cpp)
target_include_directories(CovExecV2Valid PRIVATE ${WT_COV_INCS})
target_compile_definitions(CovExecV2Valid PRIVATE COV_EXEC_PLUGIN_MODE=0)
set_target_properties(CovExecV2Valid PROPERTIES
	CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
	PREFIX ""
	SUFFIX "${WT_COV_EXEC_V2_PLUGIN_SUFFIX}"
	OUTPUT_NAME "valid"
	LIBRARY_OUTPUT_DIRECTORY "${WT_COV_EXEC_V2_PLUGIN_DIR}"
	RUNTIME_OUTPUT_DIRECTORY "${WT_COV_EXEC_V2_PLUGIN_DIR}")
if(TARGET CoverageTests_btcore)
	add_dependencies(CoverageTests_btcore CovExecV2Valid)
	target_compile_definitions(CoverageTests_btcore PRIVATE
		"WT_N04_EXEC_V2_VALID_MODULE=\"$<TARGET_FILE:CovExecV2Valid>\"")
endif()
if(TARGET CoverageTests_btporter)
	add_dependencies(CoverageTests_btporter CovExecV2Valid)
endif()
add_library(CovExecLegacy MODULE v2_exec_plugin/legacy_exec_plugin.cpp)
target_include_directories(CovExecLegacy PRIVATE ${WT_COV_INCS})
set_target_properties(CovExecLegacy PROPERTIES
	CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
	PREFIX ""
	SUFFIX "${WT_COV_EXEC_V2_PLUGIN_SUFFIX}"
	OUTPUT_NAME "legacy"
	LIBRARY_OUTPUT_DIRECTORY "${WT_COV_EXEC_V2_PLUGIN_DIR}"
	RUNTIME_OUTPUT_DIRECTORY "${WT_COV_EXEC_V2_PLUGIN_DIR}")

add_executable(CoverageTests_execv2
	v2_exec_plugin/v2_exec_plugin_factory_test.cpp
	v2_exec_plugin/v2_exec_plugin_quiesce_lifecycle_test.cpp
	v2_exec_plugin/init_fail_drains_worker_test.cpp
	v2_exec_plugin/s3_k41_identity_diagnostic_test.cpp)
target_include_directories(CoverageTests_execv2 PRIVATE ${WT_COV_INCS})
target_link_libraries(CoverageTests_execv2 PRIVATE
	GTest::gtest_main Threads::Threads WtCore WTSTools WTSUtils boost_filesystem boost_thread)
if(CMAKE_DL_LIBS)
	target_link_libraries(CoverageTests_execv2 PRIVATE ${CMAKE_DL_LIBS})
endif()
target_compile_definitions(CoverageTests_execv2 PRIVATE
	"WT_COV_EXEC_V2_LEGACY_PATH=\"$<TARGET_FILE:CovExecLegacy>\""
	"WT_COV_EXEC_V2_BAD_VERSION_PATH=\"$<TARGET_FILE:CovExecV2BadVersion>\""
	"WT_COV_EXEC_V2_BAD_SIZE_PATH=\"$<TARGET_FILE:CovExecV2BadSize>\""
	"WT_COV_EXEC_V2_BAD_CALLBACK_PATH=\"$<TARGET_FILE:CovExecV2BadCallback>\""
	"WT_COV_EXEC_V2_ABI20_PATH=\"$<TARGET_FILE:CovExecV2Abi20>\""
	"WT_COV_EXEC_V2_NO_QUIESCE_CAPABILITY_PATH=\"$<TARGET_FILE:CovExecV2NoQuiesceCapability>\""
	"WT_COV_EXEC_V2_NO_WAIT_QUIESCE_PATH=\"$<TARGET_FILE:CovExecV2NoWaitQuiesce>\""
	"WT_COV_EXEC_V2_NO_CAN_WAIT_QUIESCE_PATH=\"$<TARGET_FILE:CovExecV2NoCanWaitQuiesce>\""
	"WT_COV_EXEC_V2_SHORT_QUIESCE_PATH=\"$<TARGET_FILE:CovExecV2ShortQuiesce>\""
	"WT_COV_EXEC_V2_NO_REQUEST_QUIESCE_PATH=\"$<TARGET_FILE:CovExecV2NoRequestQuiesce>\""
	"WT_COV_EXEC_V2_CAN_WAIT_FALSE_PATH=\"$<TARGET_FILE:CovExecV2CanWaitFalse>\""
	"WT_COV_EXEC_V2_CAN_WAIT_INVALID_PATH=\"$<TARGET_FILE:CovExecV2CanWaitInvalid>\""
	"WT_COV_EXEC_V2_REQUEST_FAIL_PATH=\"$<TARGET_FILE:CovExecV2RequestFail>\""
	"WT_COV_EXEC_V2_WAIT_FAIL_PATH=\"$<TARGET_FILE:CovExecV2WaitFail>\""
	"WT_COV_EXEC_V2_REQUEST_THROW_PATH=\"$<TARGET_FILE:CovExecV2RequestThrow>\""
	"WT_COV_EXEC_V2_CAN_WAIT_THROW_PATH=\"$<TARGET_FILE:CovExecV2CanWaitThrow>\""
	"WT_COV_EXEC_V2_WAIT_THROW_PATH=\"$<TARGET_FILE:CovExecV2WaitThrow>\""
	"WT_COV_EXEC_V2_INIT_RACE_PATH=\"$<TARGET_FILE:CovExecV2InitRace>\""
	"WT_COV_EXEC_V2_ABI21_PATH=\"$<TARGET_FILE:CovExecV2Abi21>\""
	"WT_COV_EXEC_V2_SHORT_FACTORY_PREFLIGHT_PATH=\"$<TARGET_FILE:CovExecV2ShortFactoryPreflight>\""
	"WT_COV_EXEC_V2_NO_FACTORY_PREFLIGHT_CAPABILITY_PATH=\"$<TARGET_FILE:CovExecV2NoFactoryPreflightCapability>\""
	"WT_COV_EXEC_V2_NO_FACTORY_PREFLIGHT_PATH=\"$<TARGET_FILE:CovExecV2NoFactoryPreflight>\""
	"WT_COV_EXEC_V2_NULL_FACTORY_PATH=\"$<TARGET_FILE:CovExecV2NullFactory>\""
	"WT_COV_EXEC_V2_FACTORY_PREFLIGHT_FALSE_PATH=\"$<TARGET_FILE:CovExecV2FactoryPreflightFalse>\""
	"WT_COV_EXEC_V2_FACTORY_PREFLIGHT_INVALID_PATH=\"$<TARGET_FILE:CovExecV2FactoryPreflightInvalid>\""
	"WT_COV_EXEC_V2_FACTORY_PREFLIGHT_THROW_PATH=\"$<TARGET_FILE:CovExecV2FactoryPreflightThrow>\""
	"WT_COV_EXEC_V2_INIT_FAIL_DRAINS_WORKER_PATH=\"$<TARGET_FILE:CovExecV2InitFailDrainsWorker>\""
	"WT_COV_EXEC_V2_INIT_FAIL_PATH=\"$<TARGET_FILE:CovExecV2InitFail>\""
	"WT_COV_EXEC_V2_VALID_PATH=\"$<TARGET_FILE:CovExecV2Valid>\""
	"WT_COV_EXEC_V2_HOST_IDENTITY_PATH=\"$<TARGET_FILE:CovExecV2HostIdentity>\"")
add_dependencies(CoverageTests_execv2
	CovExecLegacy CovExecV2BadVersion CovExecV2BadSize CovExecV2InitFail
	CovExecV2BadCallback CovExecV2Abi20 CovExecV2NoQuiesceCapability
	CovExecV2NoWaitQuiesce CovExecV2NoCanWaitQuiesce CovExecV2ShortQuiesce
	CovExecV2NoRequestQuiesce CovExecV2CanWaitFalse CovExecV2CanWaitInvalid
	CovExecV2RequestFail CovExecV2WaitFail CovExecV2RequestThrow
	CovExecV2CanWaitThrow CovExecV2WaitThrow CovExecV2InitRace CovExecV2Abi21
	CovExecV2ShortFactoryPreflight CovExecV2NoFactoryPreflightCapability
	CovExecV2NoFactoryPreflight CovExecV2FactoryPreflightFalse
	CovExecV2FactoryPreflightInvalid CovExecV2FactoryPreflightThrow
	CovExecV2NullFactory
	CovExecV2InitFailDrainsWorker CovExecV2Valid CovExecV2HostIdentity)
set_target_properties(CoverageTests_execv2 PROPERTIES
	CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
list(APPEND MOVED_SRCS
	"${CMAKE_CURRENT_SOURCE_DIR}/v2_exec_plugin/v2_exec_plugin_factory_test.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/v2_exec_plugin/v2_exec_plugin_quiesce_lifecycle_test.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/v2_exec_plugin/init_fail_drains_worker_test.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/v2_exec_plugin/s3_k41_identity_diagnostic_test.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/v2_exec_plugin/init_fail_drains_worker_fixture.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/v2_exec_plugin/fake_exec_plugin.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/v2_exec_plugin/legacy_exec_plugin.cpp")

# ---------------------------------------------------------------------------
# 策略工厂族：需要显式控制，因为冲突形状比普通插件更细
#
# 实测（nm + grep）：
#  * `WtCtaStraFact.h:22` 与 `WtCtaTwoMaFact.h:22` 在全局命名空间里各自定义了**同名**
#    `class WtStraFact`，两个工厂 .cpp 又各发出 `_FACT_NAME` 与 `WtStraFact::*` 强符号
#    → 两族的工厂 .cpp 不能同处一个二进制（这是产品侧 ODR 缺陷，见 BUG-L00-003/005）。
#  * 策略实现 .cpp（WtStraTwoMa / WtStraDualThrust）只 `extern const char* FACT_NAME;`，
#    所以可以共存，只要全局恰好有一个 FACT_NAME 定义。
#  * WtSelStraFact / WtUftStraFact / WtHftStraFact 的类名互不相同，冲突只在 `_FACT_NAME`，
#    于是按目录给它编译期别名，让每族与自己的策略 .cpp 配对，
#    用例里 `fact.getName() == stra.getFactName()` 的身份断言仍然成立。
set(CTA_FACTORY_TUS
	"${CMAKE_SOURCE_DIR}/WtCtaTwoMaFact/WtCtaTwoMaFact.cpp"
	"${CMAKE_SOURCE_DIR}/WtCtaTwoMaFact/WtStraTwoMa.cpp"
	"${CMAKE_SOURCE_DIR}/WtCtaStraFact/WtStraDualThrust.cpp")
set(CTA_FACTORY_TESTS
	"${CMAKE_CURRENT_SOURCE_DIR}/n06_strategies_mq_share/n06_cta_stra_test.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/n06_strategies_mq_share/n06_cta_ctx_mock_test.cpp")
# lane R125（第二十波）：策略工厂/策略实现补测目录。整目录挂进对应目标，
# 并随 CTA_FACTORY_TESTS/SELHFT_TESTS 一起进 MOVED_SRCS，避免同一份用例又被主目标收走。
file(GLOB CTA_FACTORY_EXTRA CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/r125_ctafact/*.cpp")
LIST(APPEND CTA_FACTORY_TESTS ${CTA_FACTORY_EXTRA})

file(GLOB SEL_TUS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/WtSelStraFact/*.cpp")
file(GLOB UFT_TUS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/WtUftStraFact/*.cpp")
file(GLOB HFT_TUS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/WtHftStraFact/*.cpp")
set(SELHFT_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/n06_strategies_mq_share/n06_sel_uft_hft_test.cpp")
file(GLOB SELHFT_EXTRA CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/r125_selfthft/*.cpp")
LIST(APPEND SELHFT_TESTS ${SELHFT_EXTRA})

# WtSelStraFact / WtHftStraFact 不在顶层 ADD_SUBDIRECTORY 里（顶层构建不产出这两个目标），
# 因此它们的源码从未参与 -Werror 清洗——实测 WtHftStraDemo.cpp 一编就报
# -Wreorder-ctor 与 10 处 -Wunused-parameter。这里只对这几份生产 TU 放宽为「告警不致命」，
# 不改源码、不改它们的构建配置，并在缺陷清单里点名这个构建范围决定。
set_source_files_properties(${SEL_TUS} ${UFT_TUS} ${HFT_TUS} PROPERTIES
	COMPILE_OPTIONS "-Wno-error;-Wno-reorder-ctor;-Wno-unused-parameter")
if(SEL_TUS)
	set_source_files_properties(${SEL_TUS} PROPERTIES COMPILE_DEFINITIONS FACT_NAME=FACT_NAME_wtsel)
endif()
# uft 与 hft 两个工厂都导出同名的 extern "C" 入口 createStrategyFact/deleteStrategyFact
# （sel 用的是 createSelStrategyFact，不冲突），实测报 duplicate symbol。用例并不调用
# 这两个 C 入口（已 grep 确认），所以同样按目录别名。
if(UFT_TUS)
	set_source_files_properties(${UFT_TUS} PROPERTIES
		COMPILE_DEFINITIONS "FACT_NAME=FACT_NAME_wtuft;createStrategyFact=createStrategyFact_wtuft;deleteStrategyFact=deleteStrategyFact_wtuft")
endif()
if(HFT_TUS)
	set_source_files_properties(${HFT_TUS} PROPERTIES
		COMPILE_DEFINITIONS "FACT_NAME=FACT_NAME_wthft;createStrategyFact=createStrategyFact_wthft;deleteStrategyFact=deleteStrategyFact_wthft")
endif()

# 这三个列表上面的 set_source_files_properties 已经写过了，这里只能在其之后追加（见函数注释）
WT_COV_ISOLATE_INST_HLP(${SEL_TUS} ${UFT_TUS} ${HFT_TUS})

# 注意：不能用 "a|b|c;d;e" 这种字符串携带列表——`;` 本身就是 CMake 的列表分隔符，
# LIST(GET bits 2 ...) 只会拿到第一个 TU，其余静默丢失（我第一版就是这么错的）。
# 所以两个目标各写一遍，列表始终按列表传递。
if(CTA_FACTORY_TESTS AND CTA_FACTORY_TUS)
	ADD_EXECUTABLE(CoverageTests_ctafact ${CTA_FACTORY_TESTS} ${CTA_FACTORY_TUS})
	TARGET_INCLUDE_DIRECTORIES(CoverageTests_ctafact PRIVATE ${WT_COV_INCS})
	TARGET_LINK_LIBRARIES(CoverageTests_ctafact PRIVATE ${WT_COV_SHARED_LIBS})
	SET_TARGET_PROPERTIES(CoverageTests_ctafact PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	LIST(APPEND MOVED_SRCS ${CTA_FACTORY_TESTS})
endif()

set(SELHFT_TUS ${SEL_TUS} ${UFT_TUS} ${HFT_TUS})
if(SELHFT_TESTS AND SELHFT_TUS)
	ADD_EXECUTABLE(CoverageTests_selfthft ${SELHFT_TESTS} ${SELHFT_TUS})
	TARGET_INCLUDE_DIRECTORIES(CoverageTests_selfthft PRIVATE ${WT_COV_INCS})
	TARGET_LINK_LIBRARIES(CoverageTests_selfthft PRIVATE ${WT_COV_SHARED_LIBS})
	SET_TARGET_PROPERTIES(CoverageTests_selfthft PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	LIST(APPEND MOVED_SRCS ${SELHFT_TESTS})
endif()

# BUG-R31-001 / SEL factory ABI integration: build the two actual WtSelStraFact
# production TUs as an isolated shared library. Do not link these TUs into either
# host test executable: that would duplicate their definitions with SELHFT_TUS.
# This mirrors src/WtSelStraFact/CMakeLists.txt's include/link/visibility contract.
set(WT_COV_SEL_PLUGIN_TUS
	"${CMAKE_SOURCE_DIR}/WtSelStraFact/WtSelStraFact.cpp"
	"${CMAKE_SOURCE_DIR}/WtSelStraFact/WtStraDtSel.cpp")
add_library(CoverageWtSelStraFactPlugin SHARED ${WT_COV_SEL_PLUGIN_TUS})
target_include_directories(CoverageWtSelStraFactPlugin PRIVATE ${WT_COV_INCS})
if(NOT WIN32)
	target_link_libraries(CoverageWtSelStraFactPlugin PRIVATE ${STDCXXFS} ${CMAKE_DL_LIBS})
endif()
set_target_properties(CoverageWtSelStraFactPlugin PROPERTIES
	CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
	CXX_VISIBILITY_PRESET hidden
	C_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sel_plugin"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sel_plugin")

# Compile an explicit generated path into both host tests and make them depend on
# the plugin target. Missing plugin output must fail the test, never silently skip.
if(NOT TARGET CoverageTests_wtcore OR NOT TARGET CoverageTests_btcore)
	message(FATAL_ERROR "SEL plugin ABI integration requires CoverageTests_wtcore and CoverageTests_btcore")
endif()
foreach(_sel_host_test CoverageTests_wtcore CoverageTests_btcore)
	target_compile_definitions(${_sel_host_test} PRIVATE
		"WT_COV_SEL_PLUGIN_PATH=\"$<TARGET_FILE:CoverageWtSelStraFactPlugin>\""
		"WT_COV_CTA_PLUGIN_PATH=\"$<TARGET_FILE:WtCtaStraFact>\"")
	add_dependencies(${_sel_host_test} CoverageWtSelStraFactPlugin)
endforeach()

# BUG-R202 HFT factory ABI integration: compile pristine copies of the actual
# production factory TUs into a shared library. The originals above have global
# source compile definitions that rename their C exports for CoverageTests_selfthft;
# copying them into the build tree keeps those test-only renames off this plugin.
set(WT_COV_HFT_PLUGIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/hft_plugin")
file(MAKE_DIRECTORY "${WT_COV_HFT_PLUGIN_DIR}")
configure_file("${CMAKE_SOURCE_DIR}/WtHftStraFact/WtHftStraFact.cpp"
	"${WT_COV_HFT_PLUGIN_DIR}/WtHftStraFact.cpp" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/WtHftStraFact/WtHftStraDemo.cpp"
	"${WT_COV_HFT_PLUGIN_DIR}/WtHftStraDemo.cpp" COPYONLY)
set(WT_COV_HFT_PLUGIN_TUS
	"${WT_COV_HFT_PLUGIN_DIR}/WtHftStraFact.cpp"
	"${WT_COV_HFT_PLUGIN_DIR}/WtHftStraDemo.cpp")
set_source_files_properties(${WT_COV_HFT_PLUGIN_TUS} PROPERTIES
	COMPILE_OPTIONS "-Wno-error;-Wno-reorder-ctor;-Wno-unused-parameter")
add_library(CoverageWtHftStraFactPlugin SHARED ${WT_COV_HFT_PLUGIN_TUS})
target_include_directories(CoverageWtHftStraFactPlugin PRIVATE
	${WT_COV_INCS} "${CMAKE_SOURCE_DIR}/WtHftStraFact")
if(NOT WIN32)
	target_link_libraries(CoverageWtHftStraFactPlugin PRIVATE ${STDCXXFS} ${CMAKE_DL_LIBS})
endif()
set_target_properties(CoverageWtHftStraFactPlugin PROPERTIES
	CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
	OUTPUT_NAME WtHftStraFact
	CXX_VISIBILITY_PRESET hidden
	C_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin")
foreach(_hft_host_test CoverageTests_wtcore CoverageTests_btcore)
	add_dependencies(${_hft_host_test} CoverageWtHftStraFactPlugin)
endforeach()

# BUG-R6-008 positive C API registration controls use actual built plugins.
# This block follows all three targets so CMake can attach build dependencies.
add_dependencies(CoverageTests_porter
	WtCtaStraFact CoverageWtSelStraFactPlugin CoverageWtHftStraFactPlugin)
target_compile_definitions(CoverageTests_porter PRIVATE
	"WT_R6_CTA_FACTORY_PLUGIN=\"$<TARGET_FILE:WtCtaStraFact>\""
	"WT_R6_SEL_FACTORY_PLUGIN=\"$<TARGET_FILE:CoverageWtSelStraFactPlugin>\""
	"WT_R6_HFT_FACTORY_PLUGIN=\"$<TARGET_FILE:CoverageWtHftStraFactPlugin>\"")

# R134 loads the actual WtDataStorage module through WtDtMgr; provide its exact
# built location instead of deriving it from the test executable's directory.
add_dependencies(CoverageTests_porter WtDataStorage)
target_compile_definitions(CoverageTests_porter PRIVATE
	"WT_COV_R134_DATA_STORAGE_MODULE=\"$<TARGET_FILE:WtDataStorage>\"")

# ---------------------------------------------------------------------------
# 带 main() 的可执行体（EX-14/EX-15 那一类）：用编译期重命名入口的方式纳入测试
# 实测通路（2026-10-27）：用构建系统自身命令加 -Dmain=<模块>_coverage_main 编译
# LoaderRunner.cpp，得到 `T loader_runner_main_probe(int, char**)` 且导出 `_main` 数为 0，
# 因此该 TU 可以和 gtest 的 main 同链进一个测试二进制，由测试按受控 argv 调用它。
# 这样不必为了测试去改产品构建（OBJECT 化与否仍可由评审另行决定）。
# 用例目录约定：src/TestUnits/coverage/m_<模块名>/*.cpp，空目录自动跳过。
SET(WT_MAIN_MODULES WtRunner WtBtRunner WtUftRunner LoaderRunner QuoteFactory WtLatencyHFT WtLatencyUFT)
# R95 exercises WtUftRunner's dynamic UFT strategy loading boundary. Keep the
# fake factory in a separate module so its C exports are reached only by dlopen.
ADD_LIBRARY(R95UftStrategy MODULE "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r95_uft_strategy.cpp")
TARGET_INCLUDE_DIRECTORIES(R95UftStrategy PRIVATE ${WT_COV_INCS})
SET_TARGET_PROPERTIES(R95UftStrategy PROPERTIES
	OUTPUT_NAME "r95_uft_strategy"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r95_uft_strategy_fixture"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/r95_uft_strategy_fixture"
	CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
IF(WIN32)
	SET_TARGET_PROPERTIES(R95UftStrategy PROPERTIES PREFIX "" SUFFIX ".dll")
ELSEIF(APPLE)
	SET_TARGET_PROPERTIES(R95UftStrategy PROPERTIES PREFIX "lib" SUFFIX ".dylib")
ELSE()
	SET_TARGET_PROPERTIES(R95UftStrategy PROPERTIES PREFIX "lib" SUFFIX ".so")
ENDIF()
FOREACH(mod ${WT_MAIN_MODULES})
	file(GLOB mod_tus CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${mod}/*.cpp")
	file(GLOB mod_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/m_${mod}/*.cpp")
	if(NOT mod_tus OR NOT mod_tests)
		continue()
	endif()
	# 只重命名生产 TU 里的 main，测试自己的 gtest_main 不受影响。
	# 名字必须与测试侧声明完全一致：统一用小写 wt_coverage_main_<模块>，
	# 早前写成 ${mod}_coverage_main（大写开头）会让探针声明对不上号，链接期才暴露。
	string(TOLOWER "${mod}" _modlc)
	set_source_files_properties(${mod_tus} PROPERTIES
		COMPILE_DEFINITIONS "main=wt_coverage_main_${_modlc}")
	WT_COV_ISOLATE_INST_HLP(${mod_tus})
	# 普通模块目标从自身 CMakeLists 读取核心静态库候选；下面几个 runner 的
	# 专属链接集则直接对应各自产品依赖，避免全局库带入同名符号实现。
	SET(WT_M_CORES "")
	FOREACH(cand WtCore WtBtCore WtUftCore WtDtCore)
		IF(cand STREQUAL "${mod}")
			CONTINUE()
		ENDIF()
		IF(EXISTS "${CMAKE_SOURCE_DIR}/${mod}/CMakeLists.txt")
			file(STRINGS "${CMAKE_SOURCE_DIR}/${mod}/CMakeLists.txt" _hitlines REGEX "(^|[^A-Za-z0-9_])${cand}([^A-Za-z0-9_]|$)")
			IF(_hitlines)
				LIST(APPEND WT_M_CORES ${cand})
			ENDIF()
		ENDIF()
	ENDFOREACH()
	MESSAGE(STATUS "coverage: CoverageTests_m_${mod} 附加核心静态库 [${WT_M_CORES}]")
	ADD_EXECUTABLE(CoverageTests_m_${mod} ${mod_tests} ${mod_tus})
	IF(mod STREQUAL "QuoteFactory")
		# R14 launches the actual executable, so build it with its isolated coverage test.
		ADD_DEPENDENCIES(CoverageTests_m_${mod} QuoteFactory)
	ENDIF()
	TARGET_INCLUDE_DIRECTORIES(CoverageTests_m_${mod} PRIVATE ${WT_COV_INCS})
	IF(mod STREQUAL "WtRunner")
		# R6's real entry success control loads the local storage module from the isolated child cwd.
		add_dependencies(CoverageTests_m_${mod} WtDataStorage WtCtaStraFact)
		target_compile_definitions(CoverageTests_m_${mod} PRIVATE
			"WT_R28_CTA_FACTORY_PLUGIN_PATH=\"$<TARGET_FILE:WtCtaStraFact>\"")
		target_compile_definitions(CoverageTests_m_${mod} PRIVATE
			"WT_R28_DATA_STORAGE_PLUGIN_PATH=\"$<TARGET_FILE:WtDataStorage>\"")
		# Match WtRunner's product link set; in particular, do not bring in
		# WtBtPorter/WtBtCore or other global coverage libraries that can supply
		# a conflicting EventNotifier implementation.
		SET(WT_RUNNER_COV_LIBS
			GTest::gtest_main Threads::Threads
			WtCore WTSTools WTSUtils)
		IF(MSVC)
			LIST(APPEND WT_RUNNER_COV_LIBS ws2_32)
		ELSE()
			LIST(APPEND WT_RUNNER_COV_LIBS
				dl pthread boost_filesystem boost_thread ${STDCXXFS})
			IF(WIN32)
				LIST(APPEND WT_RUNNER_COV_LIBS ws2_32 iconv)
			ENDIF()
		ENDIF()
		LIST(APPEND WT_RUNNER_COV_LIBS ${ICONV_LIBRARY})
		TARGET_LINK_LIBRARIES(CoverageTests_m_${mod} PRIVATE ${WT_RUNNER_COV_LIBS})
	ELSEIF(mod STREQUAL "WtUftRunner")
		# WtUftRunner 的产品依赖只含 UFT 核心和基础工具库。不要把全局
		# WT_COV_SHARED_LIBS 中的 WtExecMon/WtPorter 链入来，以免其引入 WtCore，
		# 与 WtUftCore 中同名的 ActionPolicyMgr 符号发生静态归档抢占。
		SET(WT_UFT_RUNNER_COV_LIBS
			GTest::gtest_main Threads::Threads
			WtUftCore WTSTools WTSUtils)
		IF(MSVC)
			LIST(APPEND WT_UFT_RUNNER_COV_LIBS ws2_32)
		ELSE()
			LIST(APPEND WT_UFT_RUNNER_COV_LIBS
				dl pthread boost_filesystem boost_thread ${STDCXXFS})
			IF(WIN32)
				LIST(APPEND WT_UFT_RUNNER_COV_LIBS ws2_32 iconv)
			ENDIF()
		ENDIF()
		# src/CMakeLists.txt also links ICONV_LIBRARY to each product executable.
		LIST(APPEND WT_UFT_RUNNER_COV_LIBS ${ICONV_LIBRARY})
		TARGET_LINK_LIBRARIES(CoverageTests_m_${mod} PRIVATE ${WT_UFT_RUNNER_COV_LIBS})
	ELSEIF(mod STREQUAL "WtLatencyUFT")
		# Match WtLatencyUFT's product link set while keeping the shared global
		# coverage libraries (notably WtExecMon/WtPorter) out of this binary.
		SET(WT_LATENCY_UFT_COV_LIBS
			GTest::gtest_main Threads::Threads
			WtUftCore WTSTools WTSUtils WtShareHelper)
		IF(MSVC)
			LIST(APPEND WT_LATENCY_UFT_COV_LIBS ws2_32)
		ELSE()
			LIST(APPEND WT_LATENCY_UFT_COV_LIBS
				dl pthread boost_thread ${STDCXXFS})
			IF(WIN32)
				LIST(APPEND WT_LATENCY_UFT_COV_LIBS ws2_32 iconv)
			ENDIF()
		ENDIF()
		LIST(APPEND WT_LATENCY_UFT_COV_LIBS ${ICONV_LIBRARY})
		TARGET_LINK_LIBRARIES(CoverageTests_m_${mod} PRIVATE ${WT_LATENCY_UFT_COV_LIBS})
	ELSE()
		TARGET_LINK_LIBRARIES(CoverageTests_m_${mod} PRIVATE ${WT_COV_SHARED_LIBS} ${WT_M_CORES})
	ENDIF()
	SET_TARGET_PROPERTIES(CoverageTests_m_${mod} PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	IF(mod STREQUAL "WtLatencyHFT" OR mod STREQUAL "WtLatencyUFT")
		TARGET_COMPILE_DEFINITIONS(CoverageTests_m_${mod} PRIVATE WT_LATENCY_TOOL_TEST_HOOKS)
	ENDIF()
	IF(mod STREQUAL "WtBtRunner")
		TARGET_COMPILE_DEFINITIONS(CoverageTests_m_${mod} PRIVATE
			"WT_COV_SEL_PLUGIN_PATH=\"$<TARGET_FILE:CoverageWtSelStraFactPlugin>\"")
		ADD_DEPENDENCIES(CoverageTests_m_${mod} CoverageWtSelStraFactPlugin)
	ENDIF()
	IF(mod STREQUAL "WtUftRunner" AND TARGET UftLifecycleFakeTrader)
		ADD_DEPENDENCIES(CoverageTests_m_${mod} UftLifecycleFakeTrader)
		TARGET_COMPILE_DEFINITIONS(CoverageTests_m_${mod} PRIVATE
			"WT_UFT_LIFECYCLE_FAKE_PLUGIN_PATH=\"$<TARGET_FILE:UftLifecycleFakeTrader>\"")
	ENDIF()
	IF(mod STREQUAL "WtUftRunner" AND TARGET R130ParserLifecycle)
		ADD_DEPENDENCIES(CoverageTests_m_${mod} R130ParserLifecycle)
		TARGET_COMPILE_DEFINITIONS(CoverageTests_m_${mod} PRIVATE
			"WT_R95_PARSER_PLUGIN_PATH=\"$<TARGET_FILE:R130ParserLifecycle>\"")
	ENDIF()
		IF(mod STREQUAL "WtUftRunner")
			ADD_DEPENDENCIES(CoverageTests_m_${mod} R95UftStrategy)
			TARGET_COMPILE_DEFINITIONS(CoverageTests_m_${mod} PRIVATE
				"WT_R95_UFT_STRATEGY_PLUGIN_PATH=\"$<TARGET_FILE:R95UftStrategy>\"")
		ENDIF()
	LIST(APPEND MOVED_SRCS ${mod_tests})
	MESSAGE(STATUS "coverage: CoverageTests_m_${mod} 建立，用例 ${mod_tests}")
ENDFOREACH()

# 主目标：移除已分派的用例，避免同一份用例带着不可满足的引用留在主二进制里
FOREACH(m ${MOVED_SRCS})
	LIST(REMOVE_ITEM COV_SRCS "${m}")
ENDFOREACH()

# Keep the WtDtPorter startup-status test in its own executable: its WtDtCore
# ParserAdapter has the same C++ name as the incompatible WtCore adapter.
SET(I03_DTPORTER_START_TEST "${CMAKE_CURRENT_SOURCE_DIR}/r22_dtporter/i03_parser_start_status_test.cpp")
LIST(REMOVE_ITEM COV_SRCS "${I03_DTPORTER_START_TEST}")
ADD_EXECUTABLE(CoverageTests_i03_dtporter_start
	"${I03_DTPORTER_START_TEST}"
	"${CMAKE_SOURCE_DIR}/WtDtPorter/WtDtRunner.cpp"
	"${CMAKE_SOURCE_DIR}/WtDtPorter/WtDtPorter.cpp"
	"${CMAKE_SOURCE_DIR}/WtDtPorter/ExpParser.cpp"
	"${CMAKE_SOURCE_DIR}/WtDtPorter/ExpDumper.cpp")
TARGET_INCLUDE_DIRECTORIES(CoverageTests_i03_dtporter_start PRIVATE ${WT_COV_INCS})
TARGET_LINK_LIBRARIES(CoverageTests_i03_dtporter_start PRIVATE
	WtDtCore WTSTools WTSUtils GTest::gtest_main Threads::Threads)
IF(MSVC)
	TARGET_SOURCES(CoverageTests_i03_dtporter_start PRIVATE "${CMAKE_SOURCE_DIR}/Common/mdump.cpp")
ELSE()
	TARGET_LINK_LIBRARIES(CoverageTests_i03_dtporter_start PRIVATE
		${CMAKE_DL_LIBS} boost_filesystem boost_thread ${STDCXXFS})
	IF(WIN32)
		TARGET_LINK_LIBRARIES(CoverageTests_i03_dtporter_start PRIVATE ws2_32 iconv)
	ELSEIF(APPLE)
		TARGET_LINK_LIBRARIES(CoverageTests_i03_dtporter_start PRIVATE ${ICONV_LIBRARY})
	ENDIF()
ENDIF()
SET_TARGET_PROPERTIES(CoverageTests_i03_dtporter_start PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# WtDataStorage / WtDtPorter 的 TU 仍编进主目标（两者互不冲突，且主目标的用例需要它们）
file(GLOB MAIN_PLUGIN_TUNS CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/WtDataStorage/*.cpp"
	"${CMAKE_SOURCE_DIR}/WtDtPorter/*.cpp")
FOREACH(t ${MAIN_PLUGIN_TUNS})
	IF(t MATCHES "/WtHelper\\.cpp$")
		LIST(REMOVE_ITEM MAIN_PLUGIN_TUNS "${t}")
	ENDIF()
ENDFOREACH()
LIST(APPEND COV_SRCS ${MAIN_PLUGIN_TUNS})

# Keep isolating legacy external inst_hlp() definitions in product TUs; the ModuleHelper
# header itself no longer contributes a conflicting external symbol.
WT_COV_ISOLATE_INST_HLP(${COV_SRCS})

ADD_EXECUTABLE(CoverageTests ${COV_SRCS})
IF(TARGET R78NoParserFixture)
	ADD_DEPENDENCIES(CoverageTests R78NoParserFixture)
	TARGET_COMPILE_DEFINITIONS(CoverageTests PRIVATE "WT_R78_NO_PARSER_FIXTURE_PATH=\"$<TARGET_FILE:R78NoParserFixture>\"")
ENDIF()
TARGET_INCLUDE_DIRECTORIES(CoverageTests PRIVATE ${WT_COV_INCS})
TARGET_LINK_LIBRARIES(CoverageTests PRIVATE ${WT_COV_SHARED_LIBS} WtDtCore WtCore)
TARGET_COMPILE_DEFINITIONS(CoverageTests PRIVATE WT_DATA_WRITER_TEST_HOOKS WT_RDM_DT_READER_TEST_HOOKS)
SET_TARGET_PROPERTIES(CoverageTests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

LIST(LENGTH COV_SRCS main_n)
MESSAGE(STATUS "coverage: CoverageTests 主目标含 ${main_n} 个源文件；分派目标 ${WT_COV_SPLIT_TARGETS}")

# Regression for BUG-L00-008. These sources live outside coverage/ so the
# WT_COV_ISOLATE_INST_HLP source-path macro cannot rename inst_hlp here. The
# fixture deliberately compiles two raw header-including TUs into one shared
# module, while the test executable includes the same header in its own TU.
# This verifies both multi-TU linking and dladdr ownership for each module.
if(UNIX)
	set(WT_COV_MODULE_HELPER_ODR_DIR "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/module_helper_odr")
	add_library(ModuleHelperOdrFixture SHARED
		"${WT_COV_MODULE_HELPER_ODR_DIR}/module_helper_tu_a.cpp"
		"${WT_COV_MODULE_HELPER_ODR_DIR}/module_helper_tu_b.cpp")
	target_include_directories(ModuleHelperOdrFixture PRIVATE ${WT_COV_INCS})
	set_target_properties(ModuleHelperOdrFixture PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	if(CMAKE_DL_LIBS)
		target_link_libraries(ModuleHelperOdrFixture PRIVATE ${CMAKE_DL_LIBS})
	endif()

	add_executable(CoverageTests_modulehelper_odr
		"${WT_COV_MODULE_HELPER_ODR_DIR}/module_helper_odr_test.cpp")
	target_include_directories(CoverageTests_modulehelper_odr PRIVATE ${WT_COV_INCS})
	target_link_libraries(CoverageTests_modulehelper_odr PRIVATE
		GTest::gtest_main Threads::Threads ModuleHelperOdrFixture ${CMAKE_DL_LIBS})
	target_compile_definitions(CoverageTests_modulehelper_odr PRIVATE
		"MODULE_HELPER_ODR_FIXTURE_PATH=\"$<TARGET_FILE:ModuleHelperOdrFixture>\""
		"MODULE_HELPER_ODR_TEST_PATH=\"$<TARGET_FILE:CoverageTests_modulehelper_odr>\"")
	set_target_properties(CoverageTests_modulehelper_odr PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
elseif(MSVC)
	# BUG-L00-010: two Windows DLLs exercise multi-TU ownership, executable
	# ownership, static initialization and unload/reload independently.
	add_subdirectory("${CMAKE_SOURCE_DIR}/TestUnits/fixtures/module_helper_odr/windows"
		"${CMAKE_CURRENT_BINARY_DIR}/module_helper_windows")
endif()

# Controlled YD module fixtures for BUG-R60-004. Keep them beside the TraderYD
# test executable because TraderYD resolves modules relative to its own binary.
if(TARGET CoverageTests_t_TraderYD)
	set(WT_COV_R60_YD_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/r60_yd")
	set(WT_COV_R60_YD_TEST_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits")
	add_library(R60YdFactoryFixture SHARED "${WT_COV_R60_YD_FIXTURE_DIR}/r60_yd_factory_fixture.cpp")
	target_include_directories(R60YdFactoryFixture PRIVATE ${WT_COV_INCS} "${CMAKE_CURRENT_SOURCE_DIR}/t_TraderYD")
	set_target_properties(R60YdFactoryFixture PROPERTIES
		PREFIX "" OUTPUT_NAME "yd_r60_factory_fixture"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_R60_YD_TEST_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_R60_YD_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_library(R60YdMissingSymbolFixture SHARED "${WT_COV_R60_YD_FIXTURE_DIR}/r60_yd_missing_symbol_fixture.cpp")
	set_target_properties(R60YdMissingSymbolFixture PROPERTIES
		PREFIX "" OUTPUT_NAME "yd_r60_missing_symbol_fixture"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_R60_YD_TEST_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_R60_YD_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

	add_dependencies(CoverageTests_t_TraderYD R60YdFactoryFixture R60YdMissingSymbolFixture)
endif()

# T1-CTPOpt-05 verifies TraderCTPOpt's own dlopen/dlsym guards. The missing
# creator fixture is placed beside its test executable because getBinDir()
# resolves modules from that directory.
if(TARGET CoverageTests_t_TraderCTPOpt)
	set(WT_COV_T1_CTPOPT_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/t1_ctpopt_init")
	set(WT_COV_T1_CTPOPT_TEST_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits")
	add_library(T1CtpOptMissingSymbolFixture MODULE "${WT_COV_T1_CTPOPT_FIXTURE_DIR}/missing_symbol.cpp")
	set_target_properties(T1CtpOptMissingSymbolFixture PROPERTIES
		PREFIX "" OUTPUT_NAME "wt_t1_ctpopt_missing_symbol"
		LIBRARY_OUTPUT_DIRECTORY "${WT_COV_T1_CTPOPT_TEST_DIR}"
		RUNTIME_OUTPUT_DIRECTORY "${WT_COV_T1_CTPOPT_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	if(WIN32)
		set_target_properties(T1CtpOptMissingSymbolFixture PROPERTIES SUFFIX ".dll")
	elseif(APPLE)
		set_target_properties(T1CtpOptMissingSymbolFixture PROPERTIES SUFFIX ".dylib")
	else()
		set_target_properties(T1CtpOptMissingSymbolFixture PROPERTIES SUFFIX ".so")
	endif()
	add_dependencies(CoverageTests_t_TraderCTPOpt T1CtpOptMissingSymbolFixture)
	target_compile_definitions(CoverageTests_t_TraderCTPOpt PRIVATE
		"WT_T1_CTPOPT_MISSING_SYMBOL_MODULE=\"wt_t1_ctpopt_missing_symbol\"")
endif()

# Isolated positive YAML startup test for CTPLoader. The executable compiles the
# loader source directly and supplies a test-only definition of
# CThostFtdcTraderApi::CreateFtdcTraderApi; it deliberately does not link the
# vendor CTP archive, so the successful run cannot create a real API or connect.
if(APPLE)
	set(T6_CTP_YAML_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/TestUnits/fixtures")
	set(T6_CTP_YAML_TEST_DIR "${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits")
	add_executable(CoverageTests_t_CTPLoaderYamlPositive
		"${CMAKE_SOURCE_DIR}/CTPLoader/CTPLoader.cpp"
		"${CMAKE_SOURCE_DIR}/CTPLoader/TraderSpi.cpp"
		"${T6_CTP_YAML_FIXTURE_DIR}/t6_ctploader_yaml_factory_stub.cpp"
		"${T6_CTP_YAML_FIXTURE_DIR}/t6_ctploader_yaml_factory_test.cpp")
	target_include_directories(CoverageTests_t_CTPLoaderYamlPositive PRIVATE
		${WT_COV_INCS}
		"${CMAKE_SOURCE_DIR}/API/CTP6.6.9")
	target_link_libraries(CoverageTests_t_CTPLoaderYamlPositive PRIVATE
		GTest::gtest_main Threads::Threads WTSTools WTSUtils
		boost_filesystem boost_thread ${CMAKE_DL_LIBS} ${ICONV_LIBRARY})
	set_target_properties(CoverageTests_t_CTPLoaderYamlPositive PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${T6_CTP_YAML_TEST_DIR}"
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endif()
