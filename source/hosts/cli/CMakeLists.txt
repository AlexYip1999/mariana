add_library(agent_cli_lib
    src/cli_app.cpp
    src/cli_config.cpp
    src/cli_options.cpp
    src/host_runner.cpp
    src/interactive_app.cpp
    src/platform_transport.cpp
)

target_include_directories(agent_cli_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(agent_cli_lib
    PUBLIC
        agent_core
        agent_model
        agent_transport
        agent_runtime
    PRIVATE
        agent_metrics
        agent_tools
        agent_traces
        agent_third_party
)

add_executable(agent-cli
    src/main.cpp
)

target_link_libraries(agent-cli PRIVATE agent_cli_lib agent_metrics agent_tools)

if(WIN32)
    target_link_libraries(agent-cli PRIVATE shell32)
endif()

# 统一输出到 build/<preset>/bin/<config>/，避免模块目录调整影响可执行文件路径
set_target_properties(agent-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 共享默认配置随构建复制到 exe 同目录；config.local.json 由本机自行覆盖
if(EXISTS "${CMAKE_SOURCE_DIR}/resources/config.json")
    add_custom_command(TARGET agent-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/resources/config.json"
            "$<TARGET_FILE_DIR:agent-cli>/config.json"
    )
endif()
