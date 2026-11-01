add_library(agent_runtime
    src/agent_runner.cpp
    src/agent_session.cpp
    src/tool_registry.cpp
)

target_include_directories(agent_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(agent_runtime
    PUBLIC
        agent_core
)

target_compile_features(agent_runtime PUBLIC cxx_std_20)
