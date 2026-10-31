#include "agent/core/version.h"

namespace agent::core
{

std::string Version()
{
    return AGENT_CORE_VERSION;
}

std::string BuildBanner()
{
    return std::string("agent-core v") + Version();
}

} // namespace agent::core
