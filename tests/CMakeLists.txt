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

add_executable(agent_cli_tests
    cli_test.cpp
    cli_options_test.cpp
)
target_include_directories(agent_cli_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_cli_tests PRIVATE agent_cli_lib agent_third_party)

if(WIN32)
    add_executable(agent_transport_winhttp_tests
        winhttp_transport_test.cpp
    )
    target_link_libraries(agent_transport_winhttp_tests PRIVATE agent_transport agent_third_party)
endif()

add_test(NAME agent_core_tests COMMAND agent_core_tests)
add_test(NAME agent_transport_tests COMMAND agent_transport_tests)
add_test(NAME agent_model_tests COMMAND agent_model_tests)
add_test(NAME agent_metrics_tests COMMAND agent_metrics_tests)
add_test(NAME agent_cli_tests COMMAND agent_cli_tests)
if(WIN32)
    add_test(NAME agent_transport_winhttp_tests COMMAND agent_transport_winhttp_tests)
endif()
