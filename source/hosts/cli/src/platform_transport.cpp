#include "agent/hosts/cli/platform_transport.h"

#if defined(_WIN32)
#include "agent/transport/winhttp_transport.h"
#endif

namespace agent::hosts::cli
{

std::unique_ptr<agent::transport::HttpTransport> CreatePlatformTransport()
{
#if defined(_WIN32)
    return std::make_unique<agent::transport::WinHttpTransport>();
#else
    // TODO: 接入 libcurl（跨平台）后实现非 Windows 平台。
    return nullptr;
#endif
}

} // namespace agent::hosts::cli
