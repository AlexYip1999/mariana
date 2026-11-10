add_executable(agent_core_tests
    smoke_test.cpp
    core_primitives_test.cpp
)
target_link_libraries(agent_core_tests PRIVATE agent_core agent_third_party)

add_executable(agent_transport_tests
    http_transport_test.cpp
)
target_include_directories(agent_transport_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_transport_tests PRIVATE agent_transport agent_third_party)

add_executable(agent_model_tests
    openai_chat_model_test.cpp
)
target_include_directories(agent_model_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_model_tests PRIVATE agent_model agent_third_party)

add_executable(agent_metrics_tests
    metrics_test.cpp
)
target_include_directories(agent_metrics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_metrics_tests PRIVATE agent_metrics agent_third_party)

add_executable(agent_runtime_tests
    runtime_test.cpp
    architecture_test.cpp
    dod_test.cpp
)
target_include_directories(agent_runtime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_runtime_tests PRIVATE agent_runtime agent_metrics agent_third_party)

add_executable(agent_tools_tests
    tools_test.cpp
)
target_include_directories(agent_tools_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_tools_tests PRIVATE agent_tools agent_third_party)

add_executable(agent_cli_tests
    cli_test.cpp
    cli_options_test.cpp
    host_runner_test.cpp
)
target_include_directories(agent_cli_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_cli_tests PRIVATE agent_cli_lib agent_metrics agent_tools agent_traces agent_third_party)

add_executable(agent_trace_tests
    trace_test.cpp
)
target_link_libraries(agent_trace_tests PRIVATE agent_traces agent_third_party)

add_executable(agent_host_reference_tests
    host_reference_test.cpp
)
target_include_directories(agent_host_reference_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_host_reference_tests PRIVATE agent_host_reference agent_third_party)

add_executable(agent_memory_tests
    memory_test.cpp
    session_memory_test.cpp
)
target_include_directories(agent_memory_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_memory_tests PRIVATE agent_memory agent_runtime agent_third_party)

if(WIN32)
    add_executable(agent_transport_winhttp_tests
        winhttp_transport_test.cpp
    )
    target_link_libraries(agent_transport_winhttp_tests PRIVATE agent_transport agent_third_party ws2_32)
    target_compile_definitions(agent_transport_winhttp_tests PRIVATE NOMINMAX)
endif()

# Solution Explorer 分组；不影响构建/ctest。
set_target_properties(agent_core_tests PROPERTIES FOLDER "Tests/Core")
set_target_properties(agent_transport_tests PROPERTIES FOLDER "Tests/Integrations/Transport")
set_target_properties(agent_model_tests PROPERTIES FOLDER "Tests/Integrations/Model")
set_target_properties(agent_metrics_tests PROPERTIES FOLDER "Tests/Plugins/Metrics")
set_target_properties(agent_runtime_tests PROPERTIES FOLDER "Tests/Runtime")
set_target_properties(agent_tools_tests PROPERTIES FOLDER "Tests/Plugins/Tools")
set_target_properties(agent_cli_tests PROPERTIES FOLDER "Tests/Hosts")
set_target_properties(agent_trace_tests PROPERTIES FOLDER "Tests/Plugins/Traces")
set_target_properties(agent_host_reference_tests PROPERTIES FOLDER "Tests/Hosts")
set_target_properties(agent_memory_tests PROPERTIES FOLDER "Tests/Plugins/Memory")
if(WIN32)
    set_target_properties(agent_transport_winhttp_tests PROPERTIES FOLDER "Tests/Integrations/Transport")
endif()

add_test(NAME agent_core_tests COMMAND agent_core_tests)
add_test(NAME agent_transport_tests COMMAND agent_transport_tests)
add_test(NAME agent_model_tests COMMAND agent_model_tests)
add_test(NAME agent_metrics_tests COMMAND agent_metrics_tests)
add_test(NAME agent_runtime_tests COMMAND agent_runtime_tests)
add_test(NAME agent_tools_tests COMMAND agent_tools_tests)
add_test(NAME agent_cli_tests COMMAND agent_cli_tests)
add_test(NAME agent_trace_tests COMMAND agent_trace_tests)
add_test(NAME agent_host_reference_tests COMMAND agent_host_reference_tests)
add_test(NAME agent_memory_tests COMMAND agent_memory_tests)
if(WIN32)
    add_test(NAME agent_transport_winhttp_tests COMMAND agent_transport_winhttp_tests)
    set_tests_properties(agent_transport_winhttp_tests PROPERTIES TIMEOUT 30)
endif()
