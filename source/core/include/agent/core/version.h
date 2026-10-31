#pragma once

#include <string>

#ifndef AGENT_CORE_VERSION
#define AGENT_CORE_VERSION "0.0.0-dev"
#endif

namespace agent::core
{

/// 版本号，例如 "0.1.0"。
std::string Version();

/// 启动横幅，例如 "agent-core v0.1.0"。
std::string BuildBanner();

} // namespace agent::core
