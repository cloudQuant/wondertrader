#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.16.0)

#2. 确定工程名
PROJECT(WtLatencyUFT LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/WtLatencyUFT)

#7. 添加源码
file(GLOB SRCS *.cpp)
file(GLOB HDRS *.h)

IF(MSVC)
	list (APPEND SRCS ../Common/mdump.cpp)
ENDIF()

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})

SET(LIBS
	WtUftCore
	WTSTools
    WTSUtils
    WtShareHelper
	)

IF (MSVC)
	LIST(APPEND LIBS ws2_32)
ELSE(GNUCC)
	LIST(APPEND LIBS
		dl
		pthread
		boost_filesystem
		boost_thread)
	IF(WIN32)
		LIST(APPEND LIBS
			ws2_32 iconv)
	ENDIF()
ENDIF()

ADD_EXECUTABLE(WtLatencyUFT ${SRCS} ${HDRS})
TARGET_LINK_LIBRARIES(WtLatencyUFT ${LIBS})

IF (MSVC)
ELSE (GNUCC)
	SET_TARGET_PROPERTIES(WtLatencyUFT PROPERTIES
        LINK_FLAGS_RELEASE -s)
ENDIF ()
