#pragma once

#include <cstddef>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace agent::transport
{

/// 一次 HTTP 请求。body 是 UTF-8 字节（通常是 JSON）。
struct HttpRequest
{
    // 目标与输入
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    // 调用限制
    // Total request deadline, including the response body. Must be positive.
    int timeout_ms = 30000;
    // A bounded response protects against oversized error pages and untrusted gateways.
    std::size_t max_response_bytes = 8 * 1024 * 1024;
    std::stop_token stop_token;
};

/// 一次 HTTP 响应。error 非空表示传输层失败（连不上、超时、TLS 失败等）。
struct HttpResponse
{
    int status = 0;
    std::string body;
    std::string error;

    bool Ok() const
    {
        return error.empty() && status >= 200 && status < 300;
    }
};

/// HTTP 传输抽象：只负责把 JSON 发到 URL 并拿回状态码/body/错误。
/// 具体实现由宿主提供（CLI 用 libcurl，UE 用 FHttpModule）。
/// 不加 I 前缀：抽象类直接用角色名，具体实现加实现名（如 CurlHttpTransport）。
class HttpTransport
{
  public:
    virtual ~HttpTransport() = default;

    virtual HttpResponse PostJson(const HttpRequest& request) = 0;
};

} // namespace agent::transport
