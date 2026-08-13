project(ProjectTests)

find_package(GTest CONFIG REQUIRED)

# vcpkg's Windows gtest is a DLL. Without this, TEST() static registrars are
# not imported and every test binary reports "0 tests" while ctest still passes.
get_target_property(_gtest_type GTest::gtest TYPE)
if(_gtest_type STREQUAL "SHARED_LIBRARY")
    add_compile_definitions(GTEST_LINKED_AS_SHARED_LIBRARY=1)
endif()

function(add_gtest_executable test_name source_file)
    add_executable(${test_name} ${source_file} test_main.cpp)
    target_link_libraries(${test_name} PRIVATE
        ${ARGN}
        GTest::gtest
        GTest::gmock
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_gtest_executable(AppConfigTests AppConfigTests.cpp NetConfig)
add_gtest_executable(ProtocolTests ProtocolTests.cpp NetHandler)
add_gtest_executable(ProtocolStressTests ProtocolStressTests.cpp NetHandler)
add_gtest_executable(MessageStoreTests MessageStoreTests.cpp NetStore)

add_executable(NetIntegrationTests
    NetIntegrationTests.cpp
    test_main.cpp
    ${CMAKE_SOURCE_DIR}/apps/NetServer/ServerApp.cpp
    ${CMAKE_SOURCE_DIR}/apps/NetServer/ServerSession.cpp
    ${CMAKE_SOURCE_DIR}/apps/NetClient/ClientApp.cpp
    ${CMAKE_SOURCE_DIR}/apps/NetClient/ClientSession.cpp
)
target_include_directories(NetIntegrationTests PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/NetServer
    ${CMAKE_SOURCE_DIR}/apps/NetClient
)
target_link_libraries(NetIntegrationTests PRIVATE
    NetConfig
    NetHandler
    NetStore
    NetClient
    GTest::gtest
    GTest::gmock
)
add_test(NAME NetIntegrationTests COMMAND NetIntegrationTests)
set_tests_properties(NetIntegrationTests PROPERTIES TIMEOUT 180)

# Off by default so gtest binaries never pick up libFuzzer's main.
# CI workflow fuzz.yml enables this with Clang.
option(ENABLE_LIBFUZZER "Build FrameCodec libFuzzer harness (Clang)" OFF)
if(ENABLE_LIBFUZZER)
    add_executable(FrameCodecFuzz fuzz/FrameCodec_try_decode_fuzzer.cpp)
    target_link_libraries(FrameCodecFuzz PRIVATE NetProto)
    target_compile_options(FrameCodecFuzz PRIVATE -fsanitize=fuzzer,address -fno-omit-frame-pointer)
    target_link_options(FrameCodecFuzz PRIVATE -fsanitize=fuzzer,address)
endif()
