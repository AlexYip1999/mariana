add_library(agent_tools
    src/read_file_tool.cpp
)

target_include_directories(agent_tools
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(agent_tools
    PUBLIC
        agent_core
    PRIVATE
        agent_third_party
)

target_compile_features(agent_tools PUBLIC cxx_std_20)
