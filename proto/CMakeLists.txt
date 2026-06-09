# CMakeLists.txt for proto directory

find_package(Protobuf REQUIRED)

protobuf_generate_cpp(PROTO_SRC PROTO_HDR mip.proto state.proto)

add_library(vnet_proto STATIC ${PROTO_SRC} ${PROTO_HDR})

target_include_directories(vnet_proto PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(vnet_proto PUBLIC protobuf::libprotobuf)
