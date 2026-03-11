# CMakeLists.txt for agent

add_executable(agent setup.cpp)

set_target_properties(agent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_link_libraries(agent PRIVATE vnet)
