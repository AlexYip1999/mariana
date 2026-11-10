#pragma once

#include <functional>
#include <utility>

#include "agent/transport/http_transport.h"

namespace agent::tests
{

/// 可编程的假传输：记录请求，按 handler 返回预设响应。用于不联网的单测。
class FakeHttpTransport : public agent::transport::HttpTransport
{
  public:
    using Handler = std::function<agent::transport::HttpResponse(const agent::transport::HttpRequest&)>;

    void SetHandler(Handler handler)
    {
        m_handler = std::move(handler);
    }

    int RequestCount() const
    {
        return m_request_count;
    }

    const agent::transport::HttpRequest& LastRequest() const
    {
        return m_last_request;
    }

    agent::transport::HttpResponse PostJson(const agent::transport::HttpRequest& request) override
    {
        ++m_request_count;
        m_last_request = request;

        if (m_handler)
        {
            return m_handler(request);
        }

        return {};
    }

  private:
    // 预设响应逻辑
    Handler m_handler;

    // 最近一次请求与累计次数
    int m_request_count = 0;
    agent::transport::HttpRequest m_last_request;
};

} // namespace agent::tests
