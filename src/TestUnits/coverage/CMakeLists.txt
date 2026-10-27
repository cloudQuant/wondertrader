# 覆盖率专用测试目标：仅在 WT_ENABLE_COVERAGE=ON 时构建
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

ADD_EXECUTABLE(CoverageTests ${COV_SRCS})

TARGET_INCLUDE_DIRECTORIES(CoverageTests PRIVATE
	${CMAKE_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/Includes
	${CMAKE_SOURCE_DIR}/Share
	${CMAKE_SOURCE_DIR}/WTSTools
	${CMAKE_SOURCE_DIR}/WTSUtils)

TARGET_LINK_LIBRARIES(CoverageTests PRIVATE
	GTest::gtest_main
	Threads::Threads
	WTSTools
	WTSUtils
	WtShareHelper
	WtDataStorage
	WtDataStorageAD
	WtIndexFactory
	WtDtCore
	WtDtHelper
	WtDtPorter
	WtDtServo
	WtBtCore
	WtBtPorter
	WtCore
	WtUftCore
	WtExeFact
	WtExecMon
	WtRiskMonFact
	WtCtaTwoMaFact
	WtCtaStraFact
	WtUftStraFact
	WtMsgQue
	WtPorter
	${ICONV_LIBRARY})

SET_TARGET_PROPERTIES(CoverageTests PROPERTIES
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED ON)
