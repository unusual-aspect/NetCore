project(NetProjectCore)

add_library(NetDbg INTERFACE)
target_include_directories(NetDbg INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

find_path(USOCKETS_INCLUDE_DIR NAMES libusockets.h)
find_library(USOCKETS_LIB NAMES uSockets usockets)
if(NOT USOCKETS_LIB OR NOT USOCKETS_INCLUDE_DIR)
    message(FATAL_ERROR "uSockets NOT found! Check if it's installed in vcpkg.")
endif()

add_subdirectory(net_proto)
add_subdirectory(net_config)
add_subdirectory(net_handler)
add_subdirectory(net_client)
add_subdirectory(net_store)

# Flow: bytes → UsRuntime → NetTransport → Session.feed → FrameCodec → ProtocolStack (Vx then V1 in ProtocolLayers)
#   argv/CLI       → NetConfig
#   protocol only  → NetProto (Opcode, FrameCodec, NetProtocol, ProtocolStack, ProtocolLayers)
#   server/session → NetHandler (pulls NetProto)
#   client app     → ClientSession in apps/NetClient (NetClient CMake target aliases NetHandler)
#   store          → NetStore
