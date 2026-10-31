#pragma once

#include <memory>

#include "agent/transport/http_transport.h"

namespace agent::hosts::cli
{

/// CLI 宿主的平台 HTTP 实现。
/// Windows：临时 WinHTTP 实现；其他平台：暂返回 nullptr，等 libcurl 接入。
/// UE 宿主不经过这里，而是用 FHttpModule 实现 HttpTransport。
std::unique_ptr<agent::transport::HttpTransport> CreatePlatformTransport();

} // namespace agent::hosts::cli
