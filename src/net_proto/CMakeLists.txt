add_library(NetProto STATIC
    FrameCodec.cpp
    NetProtocol.cpp
    ProtocolLayers.cpp
)

target_include_directories(NetProto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(magic_enum CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
target_link_libraries(NetProto PUBLIC
    NetDbg
    magic_enum::magic_enum
    reflectcpp::reflectcpp
)
