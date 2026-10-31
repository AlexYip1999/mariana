cmake_minimum_required(VERSION 3.25)

project(agent_core
    VERSION 0.1.0
    DESCRIPTION "Host-agnostic agent core"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 第三方单头文件库（nlohmann/json、doctest），只提供 include 路径
add_library(agent_third_party INTERFACE)
target_include_directories(agent_third_party INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# 模块：依赖方向 hosts -> adapters -> core
add_subdirectory(source/core)
add_subdirectory(source/adapters/transport)
add_subdirectory(source/hosts/cli)

option(AGENT_CORE_BUILD_TESTS "Build agent-core tests" ON)
if(AGENT_CORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
