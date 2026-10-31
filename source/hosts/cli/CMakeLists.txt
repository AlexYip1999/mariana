add_executable(agent-cli
    src/main.cpp
)

target_link_libraries(agent-cli PRIVATE agent_core)

# 统一输出到 build/<preset>/bin/<config>/，避免模块目录调整影响可执行文件路径
set_target_properties(agent-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
