add_library(agent_transport INTERFACE)

target_include_directories(agent_transport
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(agent_transport INTERFACE cxx_std_20)

# 具体实现按宿主平台接入：
# - CLI / 服务端：libcurl（待接入）
# - UE 宿主：FHttpModule（阶段 B）
