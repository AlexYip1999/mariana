add_executable(agent_core_tests
    smoke_test.cpp
)
target_link_libraries(agent_core_tests PRIVATE agent_core agent_third_party)

add_executable(agent_transport_tests
    http_transport_test.cpp
)
target_include_directories(agent_transport_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(agent_transport_tests PRIVATE agent_transport agent_third_party)

add_test(NAME agent_core_tests COMMAND agent_core_tests)
add_test(NAME agent_transport_tests COMMAND agent_transport_tests)
