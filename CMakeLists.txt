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
add_subdirectory(source/plugins/memory)
add_subdirectory(source/runtime)
add_subdirectory(source/hosts/cli)
add_subdirectory(source/hosts/reference)

option(AGENT_CORE_BUILD_TESTS "Build agent-core tests" ON)
if(AGENT_CORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
add_subdirectory(source/eval)

# 安装规则：提供给 UE 等外部宿主的静态库 + 公共头文件 + 交付文档。
# CLI 宿主（agent-cli / agent_cli_lib）不安装；宿主无关库之外由 Host 自行实现 transport。
include(GNUInstallDirs)

set(AGENT_CORE_INSTALL_LIBRARIES
    agent_core
    agent_runtime
    agent_model
    agent_metrics
    agent_tools
    agent_traces
    agent_memory
)
if(WIN32)
    # 非 Windows 下 agent_transport 是 INTERFACE 库，没有安装产物。
    list(APPEND AGENT_CORE_INSTALL_LIBRARIES agent_transport)
endif()

install(TARGETS ${AGENT_CORE_INSTALL_LIBRARIES}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY
    source/core/include/
    source/integrations/transport/include/
    source/integrations/model/include/
    source/plugins/metrics/include/
    source/plugins/tools/include/
    source/plugins/traces/include/
    source/plugins/memory/include/
    source/runtime/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES README.md LICENSE NOTICE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/agent-core
)
install(FILES docs/build-delivery.md docs/hosts-unreal.md docs/context-provider.md docs/api.md docs/memory.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/agent-core/docs
)

# 默认启动项目；其余 target 在 Solution Explorer 中按 FOLDER 分组。
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT agent-cli)
