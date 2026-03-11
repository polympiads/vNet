# CMakeLists.txt for switch

add_executable(switch setup.cpp)

set_target_properties(switch PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_link_libraries(switch PRIVATE vnet)
