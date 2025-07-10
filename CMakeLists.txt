cmake_minimum_required(VERSION 3.10)

project(type3_network)

find_package(Threads REQUIRED)

add_executable(${PROJECT_NAME}
	net_work.cpp
    3rdparty/crc32/src/crc32.c
)

if (WIN32)
    set(SOCKPP_BUILD_SHARED OFF)
    set(SOCKPP_BUILD_STATIC ON)
	add_subdirectory(3rdparty/sockpp)
    set(SOCKPP_BUILD_SHARED OFF)
    set(SOCKPP_BUILD_STATIC ON)
endif()

include_directories(
	${SOCKPP_INCLUDE_DIR}
	${THREAD_INLCUDE_DIRS}
	${PROJECT_SOURCE_DIR}/3rdparty/crc32/src
	${PROJECT_SOURCE_DIR}/3rdparty/sockpp/include
)

include(CMakePrintHelpers)
cmake_print_variables(SOCKPP_INCLUDE_DIR)

if (CMAKE_BUILD_TYPE STREQUAL "Release")
	include(CheckIPOSupported)
	check_ipo_supported(RESULT supported OUTPUT error)
	if (supported)
		set_property(TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
	endif()
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 23)

if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W4)
	add_definitions(-DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS)
endif()

if (MSVC)
	target_link_libraries(${PROJECT_NAME}
		${CMAKE_THREAD_LIBS_INIT}
		sockpp-static
	)
endif()
