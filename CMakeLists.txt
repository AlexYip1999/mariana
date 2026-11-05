cmake_minimum_required(VERSION 3.25)

project(agent_core
    VERSION 0.1.0
    DESCRIPTION "Host-agnostic agent core"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Visual Studio Solution Explorer：允许 target 使用 FOLDER 分组。
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 第三方单头文件库（nlohmann/json、doctest），只提供 include 路径
add_library(agent_third_party INTERFACE)
target_include_directories(agent_third_party INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# 模块：依赖方向 hosts -> runtime/plugins/integrations -> core
add_subdirectory(source/core)
add_subdirectory(source/integrations/transport)
add_subdirectory(source/integrations/model)
add_subdirectory(source/plugins/metrics)
add_subdirectory(source/plugins/tools)
add_subdirectory(source/plugins/traces)
add_subdirectory(source/runtime)
add_subdirectory(source/hosts/cli)

option(AGENT_CORE_BUILD_TESTS "Build agent-core tests" ON)
if(AGENT_CORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# 默认启动项目；其余 target 在 Solution Explorer 中按 FOLDER 分组。
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT agent-cli)
