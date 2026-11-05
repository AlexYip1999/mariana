#include "agent/model/openai_chat_model.h"

#include <cstdint>
#include <set>
#include <stdexcept>
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

nlohmann::json BuildToolCallJson(const core::ToolCall& call)
{
    nlohmann::json function;
    function["name"] = call.name;
    function["arguments"] = call.arguments_json.empty() ? "{}" : call.arguments_json;

    nlohmann::json tool_call;
    tool_call["id"] = call.id;
    tool_call["type"] = "function";
    tool_call["function"] = std::move(function);
    return tool_call;
}

std::string DumpJson(const nlohmann::json& value)
{
    // 工具读到二进制/本地编码文本时，不能让 dump 因非法 UTF-8 抛异常终止进程。
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace

OpenAiChatModel::OpenAiChatModel(std::shared_ptr<transport::HttpTransport> transport, OpenAiConfig config)
    : m_transport(std::move(transport))
    , m_config(std::move(config))
{
    if (!m_transport)
    {
        throw std::invalid_argument("model requires an owning transport dependency");
    }
}

core::ChatResponse OpenAiChatModel::Chat(const core::ChatRequest& request)
{
    core::ChatResponse response;
    response.error_code = core::CallError::ModelFailure;
    if (request.call.Interruption() != core::CallError::None)
    {
        response.error_code = request.call.Interruption();
        response.error =
            response.error_code == core::CallError::Cancelled ? "model request cancelled" : "model deadline exceeded";
        return response;
    }
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
        nlohmann::json message_json;
        message_json["role"] = RoleToString(message.role);
        message_json["content"] = message.content;

        if (message.role == core::ChatRole::Assistant && !message.tool_calls.empty())
        {
            nlohmann::json tool_calls = nlohmann::json::array();
            for (const core::ToolCall& call : message.tool_calls)
            {
                tool_calls.push_back(BuildToolCallJson(call));
            }
            message_json["tool_calls"] = std::move(tool_calls);
        }
        if (message.role == core::ChatRole::Tool)
        {
            message_json["tool_call_id"] = message.tool_call_id;
        }

        body["messages"].push_back(std::move(message_json));
    }

    if (!request.tools.empty())
    {
        try
        {
            nlohmann::json tools = nlohmann::json::array();
            for (const core::ToolSpec& spec : request.tools)
            {
                nlohmann::json parameters;
                if (spec.parameters_json.empty())
                {
                    parameters["type"] = "object";
                    parameters["properties"] = nlohmann::json::object();
                }
                else
                {
                    parameters = nlohmann::json::parse(spec.parameters_json);
                }

                nlohmann::json function;
                function["name"] = spec.name;
                function["description"] = spec.description;
                function["parameters"] = std::move(parameters);

                nlohmann::json tool;
                tool["type"] = "function";
                tool["function"] = std::move(function);
                tools.push_back(std::move(tool));
            }
            body["tools"] = std::move(tools);
        }
        catch (const nlohmann::json::exception& error)
        {
            response.error = std::string("invalid tool parameters: ") + error.what();
            return response;
        }
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
    http_request.body = DumpJson(body);
    http_request.timeout_ms = request.call.RemainingTimeoutMs(m_config.timeout_ms);
    http_request.stop_token = request.call.stop_token;
    if (http_request.timeout_ms <= 0)
    {
        response.error_code = core::CallError::DeadlineExceeded;
        response.error = "model deadline exceeded";
        return response;
    }

    const transport::HttpResponse http_response = m_transport->PostJson(http_request);
    if (request.call.Interruption() != core::CallError::None)
    {
        response.error_code = request.call.Interruption();
        response.error =
            response.error_code == core::CallError::Cancelled ? "model request cancelled" : "model deadline exceeded";
        return response;
    }
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

        const nlohmann::json& choice = parsed["choices"][0];
        if (!choice.is_object() || !choice.contains("message") || !choice["message"].is_object())
        {
            response.error = "invalid response: choice.message missing";
            return response;
        }

        if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
        {
            response.finish_reason = choice["finish_reason"].get<std::string>();
        }

        const nlohmann::json& message = choice["message"];
        if (message.contains("tool_calls") && !message["tool_calls"].is_null())
        {
            if (!message["tool_calls"].is_array())
            {
                response.error = "invalid tool_calls: not an array";
                return response;
            }

            std::set<std::string> call_ids;
            for (const nlohmann::json& item : message["tool_calls"])
            {
                if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
                    !item.contains("function") || !item["function"].is_object())
                {
                    response.error = "invalid tool_calls: missing id or function";
                    return response;
                }

                const nlohmann::json& function = item["function"];
                if (!function.contains("name") || !function["name"].is_string())
                {
                    response.error = "invalid tool_calls: missing function name";
                    return response;
                }

                core::ToolCall call;
                call.id = item["id"].get<std::string>();
                call.name = function["name"].get<std::string>();
                if (call.id.empty() || !core::IsValidToolName(call.name))
                {
                    response.error = "invalid tool_calls: bad id or function name";
                    return response;
                }
                if (!call_ids.insert(call.id).second)
                {
                    response.error = "invalid tool_calls: duplicate id";
                    return response;
                }

                if (function.contains("arguments"))
                {
                    if (function["arguments"].is_string())
                    {
                        call.arguments_json = function["arguments"].get<std::string>();
                    }
                    else if (function["arguments"].is_object())
                    {
                        call.arguments_json = DumpJson(function["arguments"]);
                    }
                    else
                    {
                        response.error = "invalid tool_calls: arguments must be string or object";
                        return response;
                    }
                }
                else
                {
                    call.arguments_json = "{}";
                }
                response.tool_calls.push_back(std::move(call));
            }
        }

        const bool has_content = message.contains("content") && message["content"].is_string();
        if (!has_content && response.tool_calls.empty())
        {
            response.error = "response has no message content";
            return response;
        }
        if (has_content)
        {
            response.content = message["content"].get<std::string>();
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
        response.error_code = core::CallError::None;
    }
    catch (const nlohmann::json::exception& error)
    {
        response.error = std::string("invalid JSON response: ") + error.what();
    }

    return response;
}

} // namespace agent::model
