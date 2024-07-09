
#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

#2. 确定工程名
PROJECT(WtCtaStraStatisticalArbitrageFact LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

find_package(pybind11 REQUIRED)
# 添加包含路径
include_directories(${pybind11_INCLUDE_DIRS})

#7. 添加源码
file(GLOB SRCS "*.cpp")
file(GLOB HDRS "*.h")

SET(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin)

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})
ADD_LIBRARY(WtCtaStraStatisticalArbitrageFact SHARED ${SRCS} ${HDRS})

SET(LIBS
	boost_filesystem
	dl
)

IF (WIN32)
ELSE (UNIX)
	TARGET_LINK_LIBRARIES(WtCtaStraStatisticalArbitrageFact ${LIBS})
	SET_TARGET_PROPERTIES(WtCtaStraStatisticalArbitrageFact PROPERTIES
		CXX_VISIBILITY_PRESET hidden
		C_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN 1
        LINK_FLAGS_RELEASE -s)
ENDIF ()
target_link_libraries(WtCtaStraStatisticalArbitrageFact PRIVATE pybind11::embed)
target_link_libraries(WtCtaStraStatisticalArbitrageFact pybind11::module)
