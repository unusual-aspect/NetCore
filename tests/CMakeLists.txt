project(ProjectTests)

find_package(GTest CONFIG REQUIRED)

function(add_gtest_executable test_name source_file)
    add_executable(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE
        ${ARGN}
        GTest::gtest_main
        GTest::gmock
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_gtest_executable(AppConfigTests AppConfigTests.cpp NetConfig)
add_gtest_executable(ProtocolTests ProtocolTests.cpp NetHandler)
add_gtest_executable(MessageStoreTests MessageStoreTests.cpp NetStore)

add_executable(NetIntegrationTests
    NetIntegrationTests.cpp
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
    GTest::gtest_main
    GTest::gmock
)
add_test(NAME NetIntegrationTests COMMAND NetIntegrationTests)
