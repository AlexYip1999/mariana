add_library(agent_core
    src/system_registry.cpp
    src/version.cpp
)

target_include_directories(agent_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(agent_core PUBLIC cxx_std_20)
target_compile_definitions(agent_core PUBLIC AGENT_CORE_VERSION="${PROJECT_VERSION}")

set_target_properties(agent_core PROPERTIES FOLDER "Source/Core")
