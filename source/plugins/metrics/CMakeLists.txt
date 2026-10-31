add_library(agent_metrics
    src/cost_estimator.cpp
    src/metrics_collector.cpp
    src/metrics_formatter.cpp
)

target_include_directories(agent_metrics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(agent_metrics
    PUBLIC
        agent_core
)

target_compile_features(agent_metrics PUBLIC cxx_std_20)
