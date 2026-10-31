add_library(agent_model
    src/openai_model_adapter.cpp
)

target_include_directories(agent_model
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(agent_model
    PUBLIC
        agent_core
        agent_transport
    PRIVATE
        agent_third_party
)
