#include "agent/core/version.h"

namespace agent::core
{

std::string Version()
{
    return AGENT_CORE_VERSION;
}

int AbiVersion()
{
    return AGENT_CORE_ABI_VERSION;
}

std::string BuildBanner()
{
    return std::string("agent-core v") + Version() + " (abi " + std::to_string(AbiVersion()) + ")";
}

} // namespace agent::core
