cmake_minimum_required(VERSION 3.25)

project(EdgePulse
        VERSION 1.0.0
        DESCRIPTION "边缘节点与设备运维平台"
        LANGUAGES CXX)

# Conan 工具链只给 Release 档设置 /MD，RelWithDebInfo 会回落到编译器默认的 /MT；
# 引擎与 GTest 的包都是 /MD，混用会在链接期报 LNK2038。本工程的所有配置统一用动态运行期。
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

include(cmake/EdgePulseWarnings.cmake)

find_package(AsynGyanis REQUIRED COMPONENTS Net Database)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

option(EDGEPULSE_BUILD_TESTS "构建单元、集成与协议测试" ON)

add_subdirectory(src)
add_subdirectory(agent)
add_subdirectory(samples)

if(EDGEPULSE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
