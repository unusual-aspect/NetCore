cmake_minimum_required(VERSION 3.22)

# Allow cmake after start.sh / start.cmd set VCPKG_ROOT (prefer: cmake -B build -S .)
if(DEFINED ENV{VCPKG_ROOT})
    set(_vcpkg_toolchain "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    if(EXISTS "${_vcpkg_toolchain}")
        set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_toolchain}" CACHE FILEPATH "vcpkg toolchain" FORCE)
    endif()
    unset(_vcpkg_toolchain)
endif()

set(VCPKG_NO_PRINT_USAGE ON)

project(NetProject LANGUAGES CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/EHsc)
endif()

add_subdirectory(src)

option(BUILD_NET_SERVER "Build NetServer app" 1)
option(BUILD_NET_CLIENT "Build NetClient app" 1)

if(BUILD_NET_SERVER)
    add_subdirectory(apps/NetServer)
endif()

if(BUILD_NET_CLIENT)
    add_subdirectory(apps/NetClient)
endif()

enable_testing()
include(CTest)
add_subdirectory(tests)
