if(WIN32)
    add_library(agent_transport STATIC
        src/winhttp_transport.cpp
    )
    target_link_libraries(agent_transport PUBLIC winhttp)
    target_include_directories(agent_transport
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_features(agent_transport PUBLIC cxx_std_20)
else()
    add_library(agent_transport INTERFACE)
    target_include_directories(agent_transport
        INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_features(agent_transport INTERFACE cxx_std_20)
endif()

set_target_properties(agent_transport PROPERTIES FOLDER "Source/Integrations/Transport")

# Windows 目前是临时 WinHTTP 实现；跨平台正式实现将换成 libcurl（CurlHttpTransport）。
# UE 宿主不链接这里，而是用 FHttpModule 实现 HttpTransport。
