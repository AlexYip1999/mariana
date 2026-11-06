#pragma once

#include <string>

#ifndef AGENT_CORE_VERSION
#define AGENT_CORE_VERSION "0.0.0-dev"
#endif

/// 二进制兼容版本：公共头文件 / 对象布局 / 链接接口发生不兼容变更时 +1。
/// Host（如 UE 插件）启动时用 agent::core::AbiVersion() 与库返回值比对，不一致应拒绝加载。
#ifndef AGENT_CORE_ABI_VERSION
#define AGENT_CORE_ABI_VERSION 1
#endif

namespace agent::core
{

/// 版本号，例如 "0.1.0"。
std::string Version();

/// 二进制兼容版本；由库编译时的 AGENT_CORE_ABI_VERSION 决定。
int AbiVersion();

/// 启动横幅，例如 "agent-core v0.1.0 (abi 1)"。
std::string BuildBanner();

} // namespace agent::core
