# CMakeLists.txt for conductor

add_executable(conductor setup.cpp)

set_target_properties(conductor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_link_libraries(conductor PRIVATE vnet)
