
#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.16.0)

#2. 确定工程名
PROJECT(TestUnits LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/TestUnits)

#7. 添加源码
file(GLOB SRCS *.cpp ./gtest/*.cc)

SET(LIBS
	WtBtCore
    WTSTools
	WTSUtils
    WtShareHelper)
IF (MSVC)
ELSE(GNUCC)
    LIST(APPEND LIBS pthread ${STDCXXFS})
	IF(WIN32)
		LIST(APPEND LIBS iconv)
	ENDIF()
ENDIF()

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})

# Bundled gtest is third-party code with its own warning policy
IF(NOT MSVC)
SET(WT_GTEST_OPTS "-w")
IF(WT_ENABLE_COVERAGE)
	LIST(APPEND WT_GTEST_OPTS -fno-profile-instr-generate -fno-coverage-mapping)
ENDIF()
FOREACH(SRC ${SRCS})
	IF(SRC MATCHES "/gtest/")
		SET_SOURCE_FILES_PROPERTIES(${SRC} PROPERTIES COMPILE_OPTIONS "${WT_GTEST_OPTS}")
	ENDIF()
ENDFOREACH()
ENDIF()

ADD_EXECUTABLE(TestUnits ${SRCS})
TARGET_INCLUDE_DIRECTORIES(TestUnits BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gtest")
# Bundled gtest template triggers this; test code keeps -Wall -Wextra otherwise
IF(NOT MSVC)
TARGET_COMPILE_OPTIONS(TestUnits PRIVATE -Wno-sign-compare)
ENDIF()
TARGET_LINK_LIBRARIES(TestUnits ${LIBS})

IF (MSVC)
ELSE (GNUCC)
	SET_TARGET_PROPERTIES(TestUnits PROPERTIES
        LINK_FLAGS_RELEASE "${WT_LINK_STRIP_FLAG}")
ENDIF ()IF(WT_ENABLE_COVERAGE)
	ADD_SUBDIRECTORY(coverage)
ENDIF()
