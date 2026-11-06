add_executable(agent-eval
    src/main.cpp
)

target_link_libraries(agent-eval
    PRIVATE
        agent_core
        agent_model
        agent_runtime
        agent_tools
        agent_transport
        agent_third_party
)

target_compile_features(agent-eval PRIVATE cxx_std_20)

# 与 agent-cli 一致：统一输出到 build/<preset>/bin/<config>/
set_target_properties(agent-eval PROPERTIES
    FOLDER "Source/Eval"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(AGENT_CORE_BUILD_TESTS)
    add_test(NAME agent_eval_mock
        COMMAND agent-eval --tasks ${CMAKE_SOURCE_DIR}/eval/tasks.json --mode mock --min-pass-rate 0.7
    )
    set_tests_properties(agent_eval_mock PROPERTIES TIMEOUT 120)
endif()
