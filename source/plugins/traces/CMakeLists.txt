add_library(agent_traces
    src/file_tracer.cpp
    src/trace_sink.cpp
    src/trace_usage.cpp
    src/trace_viewer.cpp
)

target_include_directories(agent_traces
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(agent_traces
    PUBLIC
        agent_core
    PRIVATE
        agent_third_party
)

target_compile_features(agent_traces PUBLIC cxx_std_20)
