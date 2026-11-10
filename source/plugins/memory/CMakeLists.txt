add_library(agent_memory
    src/in_memory_store.cpp
    src/jsonl_memory_store.cpp
    src/model_summarizer.cpp
    src/rule_based_summarizer.cpp
)

target_include_directories(agent_memory
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(agent_memory
    PUBLIC
        agent_core
    PRIVATE
        agent_third_party
)

target_compile_features(agent_memory PUBLIC cxx_std_20)

set_target_properties(agent_memory PROPERTIES FOLDER "Source/Plugins/Memory")
