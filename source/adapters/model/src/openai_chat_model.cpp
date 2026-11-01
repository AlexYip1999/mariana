#include "agent/model/openai_chat_model.h"

#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace agent::model
{

namespace
{

const char* RoleToString(core::ChatRole role)
{
    switch (role)
    {
    case core::ChatRole::System:
        return "system";
    case core::ChatRole::User:
        return "user";
    case core::ChatRole::Assistant:
        return "assistant";
    case core::ChatRole::Tool:
        return "tool";
    }
    return "user";
}

std::string FormatHttpError(const transport::HttpResponse& response)
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

std::int64_t ReadUsageInt(const nlohmann::json& usage, const char* key)
{
    if (!usage.contains(key) || !usage[key].is_number_integer())
    {
        return 0;
    }
    return usage[key].get<std::int64_t>();
}

} // namespace

OpenAiChatModel::OpenAiChatModel(transport::HttpTransport& transport, OpenAiConfig config)
    : m_transport(transport)
    , m_config(std::move(config))
{}

core::ChatResponse OpenAiChatModel::Chat(const core::ChatRequest& request)
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
    http_request.body = body.dump();
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

        if (parsed.contains("usage") && parsed["usage"].is_object())
        {
            const nlohmann::json& usage = parsed["usage"];
            response.usage.prompt_tokens = ReadUsageInt(usage, "prompt_tokens");
            response.usage.completion_tokens = ReadUsageInt(usage, "completion_tokens");
            response.usage.total_tokens = ReadUsageInt(usage, "total_tokens");
            response.usage.prompt_cache_hit_tokens = ReadUsageInt(usage, "prompt_cache_hit_tokens");
            response.usage.prompt_cache_miss_tokens = ReadUsageInt(usage, "prompt_cache_miss_tokens");

            // OpenAI 兼容接口把缓存命中放在 prompt_tokens_details.cached_tokens。
            if (response.usage.prompt_cache_hit_tokens == 0 && usage.contains("prompt_tokens_details") &&
                usage["prompt_tokens_details"].is_object())
            {
                response.usage.prompt_cache_hit_tokens = ReadUsageInt(usage["prompt_tokens_details"], "cached_tokens");
            }
            if (response.usage.prompt_cache_miss_tokens == 0 &&
                response.usage.prompt_tokens > response.usage.prompt_cache_hit_tokens)
            {
                response.usage.prompt_cache_miss_tokens =
                    response.usage.prompt_tokens - response.usage.prompt_cache_hit_tokens;
            }
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
