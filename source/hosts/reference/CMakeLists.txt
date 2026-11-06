add_library(agent_host_reference
    src/reference_host.cpp
)

target_include_directories(agent_host_reference
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(agent_host_reference
    PUBLIC
        agent_core
        agent_runtime
        agent_model
        agent_transport
)

target_compile_features(agent_host_reference PUBLIC cxx_std_20)

set_target_properties(agent_host_reference PROPERTIES FOLDER "Source/Hosts/Reference")
