# CMakeLists.txt for operator tools

add_executable(vnet-state state.cpp)

set_target_properties(vnet-state PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_link_libraries(vnet-state PRIVATE vnet_proto)
