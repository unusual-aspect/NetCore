add_library(NetConfig STATIC
    AppConfig.cpp
)

target_include_directories(NetConfig PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(cxxopts CONFIG REQUIRED)

target_link_libraries(NetConfig PUBLIC
    NetDbg
    NetProto
    cxxopts::cxxopts
)
