#include "agent/model/openai_model_adapter.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace agent::model
{

namespace
{

const char* RoleToString(agent::core::ChatRole role)
{
    switch (role)
    {
    case agent::core::ChatRole::System:
        return "system";
    case agent::core::ChatRole::User:
        return "user";
    case agent::core::ChatRole::Assistant:
        return "assistant";
    case agent::core::ChatRole::Tool:
        return "tool";
    }
    return "user";
}

std::string FormatHttpError(const agent::transport::HttpResponse& response)
{
    std::string message = "HTTP " + std::to_string(response.status);
    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(response.body);
        if (parsed.contains("error") && parsed["error"].is_object() && parsed["error"].contains("message") &&
            parsed["error"]["message"].is_string())
        {
            message += ": " + parsed["error"]["message"].get<std::string>();
        }
    }
    catch (const nlohmann::json::exception&)
    {
        // 非 JSON 错误体：保留状态码即可。
    }
    return message;
}

} // namespace

OpenAiModelAdapter::OpenAiModelAdapter(transport::HttpTransport& transport, OpenAiConfig config)
    : m_transport(transport), m_config(std::move(config))
{
}

core::ChatResponse OpenAiModelAdapter::Chat(const core::ChatRequest& request)
{
    core::ChatResponse response;
    if (m_config.api_key.empty())
    {
        response.error = "API key is empty";
        return response;
    }

    nlohmann::json body;
    body["model"] = request.model.empty() ? m_config.model : request.model;
    body["messages"] = nlohmann::json::array();
    for (const core::ChatMessage& message : request.messages)
    {
        body["messages"].push_back({{"role", RoleToString(message.role)}, {"content", message.content}});
    }
    body["temperature"] = request.temperature;
    body["max_tokens"] = request.max_tokens;
    body["stream"] = false;

    transport::HttpRequest http_request;
    http_request.url = m_config.endpoint;
    http_request.body = body.dump();
    http_request.headers = {
        {
            "Content-Type",
            "application/json",
        },
        {
            "Authorization",
            "Bearer " + m_config.api_key,
        },
    };
    http_request.timeout_ms = m_config.timeout_ms;

    const transport::HttpResponse http_response = m_transport.PostJson(http_request);
    if (!http_response.error.empty())
    {
        response.error = http_response.error;
        return response;
    }
    if (!http_response.Ok())
    {
        response.error = FormatHttpError(http_response);
        return response;
    }

    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(http_response.body);
        if (!parsed.contains("choices") || !parsed["choices"].is_array() || parsed["choices"].empty())
        {
            response.error = "response has no choices";
            return response;
        }

        const nlohmann::json& message = parsed["choices"][0]["message"];
        if (!message.contains("content") || !message["content"].is_string())
        {
            response.error = "response has no message content";
            return response;
        }

        response.ok = true;
        response.content = message["content"].get<std::string>();
    }
    catch (const nlohmann::json::exception& error)
    {
        response.error = std::string("invalid JSON response: ") + error.what();
    }

    return response;
}

} // namespace agent::model
