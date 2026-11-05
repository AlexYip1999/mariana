add_library(agent_runtime
    src/agent.cpp
    src/agent_run.cpp
    src/agent_runner.cpp
    src/agent_session.cpp
    src/composition.cpp
    src/context_policy.cpp
    src/execution_id.cpp
    src/run_context.cpp
    src/tool_output_policy.cpp
    src/tool_registry.cpp
    src/tool_dispatcher.cpp
    src/turn_executor.cpp
    src/turn_transaction.cpp
    src/system_notifier.cpp
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

set_target_properties(agent_runtime PROPERTIES FOLDER "Source/Runtime")
