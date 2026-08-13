add_library(NetStore STATIC
    MessageStore.cpp
    StoreWorker.cpp
)

target_include_directories(NetStore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(NetStore PUBLIC
    NetDbg
    unofficial::sqlite3::sqlite3
    Threads::Threads
)
