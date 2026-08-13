add_library(NetHandler STATIC
    AbstractNetSession.cpp
    UsRuntime.cpp
    NetTransport.cpp
)

target_include_directories(NetHandler PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(USOCKETS_INCLUDE_DIR)
    target_include_directories(NetHandler PUBLIC ${USOCKETS_INCLUDE_DIR})
endif()

find_package(libuv CONFIG REQUIRED)

target_link_libraries(NetHandler PUBLIC
    NetProto
    ${USOCKETS_LIB}
    $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
)

target_compile_definitions(NetHandler PUBLIC LIBUS_USE_LIBUV)

if(WIN32)
    target_link_libraries(NetHandler PUBLIC ws2_32)
endif()
