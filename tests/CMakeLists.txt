
include(CTest)
set(CTEST_TIMEOUT 30)

include(GoogleTest)

function(create_test test_name test_file)
    add_executable(${test_name} ${test_file} utils/malloc_wrapper.cpp utils/tun_wrapper.cpp)
    target_include_directories(${test_name} PRIVATE .)
    target_link_libraries(${test_name} PRIVATE vnet GTest::gtest_main GTest::gmock)
    target_link_options(${test_name} PRIVATE -Wl,--wrap,malloc -Wl,--wrap,read -Wl,--wrap,write)
    gtest_discover_tests(${test_name})
endfunction()

# vnet/protocol
create_test(header_tests  protocol/header_tests.cpp)
create_test(types_tests   protocol/types_tests.cpp)

# vnet/netqueue
create_test(element_tests netqueue/element_tests.cpp)

create_test(netqueue_socket_tests netqueue/netqueue_socket_tests.cpp)
create_test(netqueue_epoll_tests  netqueue/netqueue_epoll_tests.cpp)
create_test(netqueue_tun_tests    netqueue/netqueue_tun_tests.cpp)
create_test(netqueue_tests        netqueue/netqueue_tests.cpp)

# testing utils
create_test(tun_wrapper_tests utils/tun_wrapper_tests.cpp)
