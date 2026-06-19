cmake_minimum_required(VERSION 3.28.0)
PROJECT(vNet)
SET(CMAKE_CXX_FLAGS "-g -Wall -Werror -std=c++17")

option(ENABLE_COVERAGE "Enable code coverage instrumentation (requires gcov/lcov)" OFF)

find_program(VALGRIND_EXECUTABLE NAMES valgrind)

if (ENABLE_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage -O0")
    set(MEMORYCHECK_COMMAND "${VALGRIND_EXECUTABLE}")
    set(MEMORYCHECK_COMMAND_OPTIONS "--tool=memcheck --leak-check=full --track-fds=yes --error-exitcode=1")

    add_custom_target(
        memcheck
        COMMAND ctest -T memcheck
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/lookup_wrong_fds.py ${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary
    )

    add_custom_target(
        coverage
        COMMAND lcov --directory . --zerocounters --rc branch_coverage=1
        COMMAND ctest

        COMMAND lcov --capture --directory . --output-file coverage.info --ignore-errors mismatch --rc branch_coverage=1
        COMMAND lcov --remove coverage.info '/usr/*' '*test*' --output-file coverage_filtered.info --rc branch_coverage=1 --ignore-errors unused
        COMMAND genhtml coverage_filtered.info --output-directory html_report --rc branch_coverage=1
    )
endif()

#############################################################
###################### PROTOCOL BUFFER ######################
#############################################################

ADD_SUBDIRECTORY(proto)
include_directories(${ProtobufIncludePath})

#############################################################
######################## VIRTUAL NET ########################
#############################################################

SET(VNET_SOURCE_FILES
        src/common/socket_utils.cpp
        src/common/tun.cpp
        
        src/protocol/header.cpp
        src/protocol/types.cpp
        src/protocol/dispatch.cpp

        src/netqueue/element.cpp
        src/netqueue/netqueue.cpp
        src/netqueue/handler.cpp

        src/blackbox/ipv4.cpp
        src/blackbox/agent_registry.cpp
        src/blackbox/routing_table.cpp
        src/blackbox/config.cpp
        src/blackbox/blackbox.cpp)

add_library(vnet STATIC ${VNET_SOURCE_FILES})
target_link_libraries(vnet PUBLIC vnet_proto)
target_link_libraries(vnet PRIVATE gcov)
target_include_directories(vnet PUBLIC include)

#############################################################
######################## EXECUTABLES ########################
#############################################################

add_subdirectory(src/conductor)
add_subdirectory(src/switch)
add_subdirectory(src/agent)

#############################################################
######################## GOOGLE TEST ########################
#############################################################

find_package(GTest QUIET)
if (NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    )
    FetchContent_MakeAvailable(googletest)
endif()
enable_testing()
add_subdirectory(tests)
