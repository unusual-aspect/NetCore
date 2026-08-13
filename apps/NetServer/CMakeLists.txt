project(NetServerApp)

add_executable(${PROJECT_NAME}
    main.cpp
    ServerApp.cpp
    ServerSession.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME NetServer)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    NetConfig
    NetHandler
    NetStore
)
