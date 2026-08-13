# Compat target: apps use ClientSession; this name remains for existing link lines.
add_library(NetClient INTERFACE)

target_link_libraries(NetClient INTERFACE
    NetHandler
)
