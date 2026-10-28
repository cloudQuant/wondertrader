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

# src/Share/ModuleHelper.hpp:48 在头文件里定义了「非 inline、外部链接」的 void inst_hlp() {}，
# 每个包含它的 TU 都会输出一份定义（TraderDD.cpp:54、TraderHTS.cpp:56 还各自又写了一份）。
# 两个这样的 TU 一旦链进同一个测试二进制就 duplicate symbol、整个目标链不通
# （实测：CoverageTests 主目标里 r16_logger_dll_test.cpp.o 撞 WtDtPorter.cpp.o）。
# 本轮不改产品代码：按源文件路径哈希给每个 TU 唯一改名。getInstPath() 本身是 static、
# 每 TU 各取自身模块地址，所以改名不改变行为。必须在其它
# set_source_files_properties(COMPILE_DEFINITIONS) 之后调用，否则会被后写的一方覆盖。
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
# 改了会波及其他可执行目标的告警面）。改用一个只作用于测试目标的软链接目录：
# clang 只对*同一个路径串*去重，所以 <builddir>/cov_boost_include/boost → Homebrew boost
# 以 -I 形态进搜索链，先于内置 /usr/local/include 命中，头与库就一致了（实测 1_88）。
SET(WT_COV_BOOST_BRIDGE "${CMAKE_CURRENT_BINARY_DIR}/cov_boost_include/boost")
IF(NOT Boost_INCLUDE_DIR)
	MESSAGE(FATAL_ERROR "coverage: Boost_INCLUDE_DIR 为空，无法钉住 boost 头版本（find_package(Boost) 是否在 src/CMakeLists 执行？）")
ENDIF()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cov_boost_include")
file(CREATE_LINK "${Boost_INCLUDE_DIR}/boost" "${WT_COV_BOOST_BRIDGE}" SYMBOLIC)

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
	"exefact|WtExeFact,WtExecMon|n05_exec_factories/n05_exe_fact_test.cpp+n05_exec_factories/n05_minimpact_calc_test.cpp+n05_exec_factories/n05_channel_ready_test.cpp+n05_exec_factories/n05_ordmon_test.cpp+dir:r7_exefact+dir:r24_exefact2"
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
	TARGET_LINK_LIBRARIES(CoverageTests_${suffix} PRIVATE ${WT_COV_SHARED_LIBS})
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

# ---------------------------------------------------------------------------
# 带 main() 的可执行体（EX-14/EX-15 那一类）：用编译期重命名入口的方式纳入测试
# 实测通路（2026-10-27）：用构建系统自身命令加 -Dmain=<模块>_coverage_main 编译
# LoaderRunner.cpp，得到 `T loader_runner_main_probe(int, char**)` 且导出 `_main` 数为 0，
# 因此该 TU 可以和 gtest 的 main 同链进一个测试二进制，由测试按受控 argv 调用它。
# 这样不必为了测试去改产品构建（OBJECT 化与否仍可由评审另行决定）。
# 用例目录约定：src/TestUnits/coverage/m_<模块名>/*.cpp，空目录自动跳过。
SET(WT_MAIN_MODULES WtRunner WtBtRunner WtUftRunner LoaderRunner QuoteFactory WtLatencyHFT WtLatencyUFT)
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
	# 该可执行体自己链哪些「核心静态库」，从它自己的 CMakeLists 里读，不手敲。
	# 实测：WtRunner 能链上是因为它的核心符号可以从 libWtPorter.dylib 里取到，
	# 而 WtUftRunner / WtLatencyUFT 用到的 `ShareManager::*` 等在共享库集合里没有出处
	# （产品那边是 `SET(LIBS WtUftCore …)`），测试目标照抄才不漏符号。
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
	TARGET_INCLUDE_DIRECTORIES(CoverageTests_m_${mod} PRIVATE ${WT_COV_INCS})
	TARGET_LINK_LIBRARIES(CoverageTests_m_${mod} PRIVATE ${WT_COV_SHARED_LIBS} ${WT_M_CORES})
	SET_TARGET_PROPERTIES(CoverageTests_m_${mod} PROPERTIES
		CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	LIST(APPEND MOVED_SRCS ${mod_tests})
	MESSAGE(STATUS "coverage: CoverageTests_m_${mod} 建立，用例 ${mod_tests}")
ENDFOREACH()

# 主目标：移除已分派的用例，避免同一份用例带着不可满足的引用留在主二进制里
FOREACH(m ${MOVED_SRCS})
	LIST(REMOVE_ITEM COV_SRCS "${m}")
ENDFOREACH()

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

# 产品头 src/Share/ModuleHelper.hpp:48 在头文件里定义了「非 inline、外部链接」的
# void inst_hlp() {}，每个包含它的 TU 都会输出一份定义。一旦两个这样的 TU 链进同一个
# 测试二进制就直接 duplicate symbol 链不通（实测：r16_logger_dll_test.cpp.o 与
# WtDtPorter.cpp.o，另有 TraderDD.cpp:54 / TraderHTS.cpp:56 自带的第三份）。
# 本轮不改产品代码：按源文件路径哈希给每个 TU 一个唯一改名，只有真的包含该头的 TU 受影响，
# getInstPath() 的语义不变（它本来就是 static、每 TU 各自取自身模块地址）。
WT_COV_ISOLATE_INST_HLP(${COV_SRCS})

ADD_EXECUTABLE(CoverageTests ${COV_SRCS})
TARGET_INCLUDE_DIRECTORIES(CoverageTests PRIVATE ${WT_COV_INCS})
TARGET_LINK_LIBRARIES(CoverageTests PRIVATE ${WT_COV_SHARED_LIBS} WtDtCore WtCore)
SET_TARGET_PROPERTIES(CoverageTests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

LIST(LENGTH COV_SRCS main_n)
MESSAGE(STATUS "coverage: CoverageTests 主目标含 ${main_n} 个源文件；分派目标 ${WT_COV_SPLIT_TARGETS}")
