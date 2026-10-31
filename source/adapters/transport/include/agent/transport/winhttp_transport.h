#pragma once

#include "agent/transport/http_transport.h"

namespace agent::transport
{

/// Windows-only 临时实现（WinHTTP），用于 CLI 真实联调。
/// 正式跨平台实现将替换为 CurlHttpTransport，接口不变。
/// 不把 WinHTTP 类型暴露到接口外。
class WinHttpTransport : public HttpTransport
{
  public:
    HttpResponse PostJson(const HttpRequest& request) override;
};

} // namespace agent::transport
