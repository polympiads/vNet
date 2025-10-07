find_package(Protobuf REQUIRED)

protobuf_generate_cpp(
    PROTO_SRC
    PROTO_HEADER
    message.proto
)

add_library(vnet_proto ${PROTO_HEADER} ${PROTO_SRC})
target_link_libraries(vnet_proto PRIVATE ${PROTOBUF_LIBRARIES})