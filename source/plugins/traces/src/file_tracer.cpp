#include "agent/plugins/traces/file_tracer.h"

#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent/core/utf8.h"

namespace agent::plugins::traces
{

namespace
{

constexpr std::size_t MAX_TRACE_TEXT = 2000;

long long ElapsedMillis(std::chrono::steady_clock::time_point start)
{
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return elapsed.count();
}

std::string DefaultSessionId()
{
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::random_device device;
    std::ostringstream stream;
    stream << stamp << "-" << device();
    return stream.str();
}

std::string RoleName(core::ChatRole role)
{
    switch (role)
    {
        case core::ChatRole::System:
        {
            return "system";
        }
        case core::ChatRole::User:
        {
            return "user";
        }
        case core::ChatRole::Assistant:
        {
            return "assistant";
        }
        case core::ChatRole::Tool:
        {
            return "tool";
        }
    }
    return "unknown";
}

nlohmann::json UsageJson(const core::ChatUsage& usage)
{
    nlohmann::json result;
    result["prompt_tokens"] = usage.prompt_tokens;
    result["completion_tokens"] = usage.completion_tokens;
    result["total_tokens"] = usage.total_tokens;
    result["prompt_cache_hit_tokens"] = usage.prompt_cache_hit_tokens;
    result["prompt_cache_miss_tokens"] = usage.prompt_cache_miss_tokens;
    return result;
}

std::string ToolCallsSummary(const std::vector<core::ToolCall>& calls)
{
    nlohmann::json result = nlohmann::json::array();
    for (const core::ToolCall& call : calls)
    {
        result.push_back({{"id", call.id}, {"name", call.name}});
    }
    return result.dump();
}

} // namespace

FileTracer::FileTracer(std::filesystem::path trace_dir, std::string session_id)
    : m_session_id(session_id.empty() ? DefaultSessionId() : std::move(session_id))
{
    m_path = trace_dir / (m_session_id + ".jsonl");
    m_sink = std::make_shared<JsonlFileSink>(m_path);
}

FileTracer::FileTracer(std::shared_ptr<TraceSink> sink)
    : m_sink(std::move(sink))
{}

FileTracer::~FileTracer()
{
    Close();
}

bool FileTracer::IsOpen() const
{
    return m_sink != nullptr && m_sink->IsOpen();
}

const std::filesystem::path& FileTracer::Path() const
{
    return m_path;
}

const std::string& FileTracer::SessionId() const
{
    return m_session_id;
}

void FileTracer::Close()
{
    if (m_sink != nullptr)
    {
        m_sink->Close();
    }
}

void FileTracer::OnTurnStart()
{
    StartTurn(nullptr);
}

void FileTracer::OnTurnStarted(const core::TurnEvent& event)
{
    StartTurn(&event);
}

void FileTracer::StartTurn(const core::TurnEvent* event)
{
    ++m_turn;
    m_step = 0;
    m_turn_started = std::chrono::steady_clock::now();

    if (event != nullptr && !event->run_id.empty())
    {
        m_run_id = event->run_id;
    }
    if (event != nullptr && !event->session_id.empty())
    {
        m_session_id = event->session_id;
    }

    nlohmann::json event_json;
    event_json["ts"] = Timestamp();
    event_json["event"] = "turn_start";
    event_json["turn"] = m_turn;
    event_json["session_id"] = m_session_id;
    event_json["run_id"] = m_run_id;

    WriteEvent(event_json.dump());
}

void FileTracer::OnModelRequest(const core::ChatRequest& request)
{
    ++m_step;
    m_request_started = std::chrono::steady_clock::now();
    m_model = request.model;

    std::string last_content;
    std::string last_role;
    if (!request.messages.empty())
    {
        last_content = request.messages.back().content;
        last_role = RoleName(request.messages.back().role);
    }

    nlohmann::json event;
    event["ts"] = Timestamp();
    event["event"] = "model_request";
    event["turn"] = m_turn;
    event["step"] = m_step;
    event["session_id"] = m_session_id;
    event["run_id"] = m_run_id;
    event["model"] = request.model;
    event["message_count"] = request.messages.size();
    event["tool_count"] = request.tools.size();
    event["last_role"] = last_role;
    event["last_content"] = Sanitize(last_content);

    WriteEvent(event.dump());
}

void FileTracer::OnModelResponse(const core::ChatResponse& response)
{
    nlohmann::json event;
    event["ts"] = Timestamp();
    event["event"] = "model_response";
    event["turn"] = m_turn;
    event["step"] = m_step;
    event["session_id"] = m_session_id;
    event["run_id"] = m_run_id;
    event["model"] = m_model;
    event["ok"] = response.ok;
    event["finish_reason"] = response.finish_reason;
    event["content"] = Sanitize(response.content);
    event["tool_calls"] = ToolCallsSummary(response.tool_calls);
    event["usage"] = UsageJson(response.usage);
    event["error"] = Sanitize(response.error);
    event["duration_ms"] = ElapsedMillis(m_request_started);

    WriteEvent(event.dump());
}

void FileTracer::OnToolCall(const core::ToolCall& call)
{
    m_tool_started = std::chrono::steady_clock::now();

    nlohmann::json event;
    event["ts"] = Timestamp();
    event["event"] = "tool_call";
    event["turn"] = m_turn;
    event["step"] = m_step;
    event["session_id"] = m_session_id;
    event["run_id"] = m_run_id;
    event["id"] = call.id;
    event["name"] = call.name;
    event["arguments"] = Sanitize(call.arguments_json);

    WriteEvent(event.dump());
}

void FileTracer::OnToolResult(const core::ToolCall& call, const core::ToolResult& result)
{
    nlohmann::json event;
    event["ts"] = Timestamp();
    event["event"] = "tool_result";
    event["turn"] = m_turn;
    event["step"] = m_step;
    event["session_id"] = m_session_id;
    event["run_id"] = m_run_id;
    event["id"] = call.id;
    event["name"] = call.name;
    event["ok"] = result.ok;
    event["content"] = Sanitize(result.ok ? result.content : result.error);
    event["duration_ms"] = ElapsedMillis(m_tool_started);

    WriteEvent(event.dump());
}

void FileTracer::OnToolReplayed(const core::ToolCall& call, const core::ToolResult& result)
{
    nlohmann::json event;
    event["ts"] = Timestamp();
    event["event"] = "tool_result";
    event["turn"] = m_turn;
    event["step"] = m_step;
    event["session_id"] = m_session_id;
    event["run_id"] = m_run_id;
    event["id"] = call.id;
    event["name"] = call.name;
    event["ok"] = result.ok;
    event["content"] = Sanitize(result.ok ? result.content : result.error);
    event["replayed"] = true;
    event["duration_ms"] = 0;

    WriteEvent(event.dump());
}

void FileTracer::OnTurnEnd(bool ok, const std::string& error, int steps)
{
    EndTurn(nullptr, ok, error, steps);
}

void FileTracer::OnTurnEnded(const core::TurnEvent& event)
{
    EndTurn(&event, event.ok, event.error, event.steps);
}

void FileTracer::EndTurn(const core::TurnEvent* event, bool ok, const std::string& error, int steps)
{
    nlohmann::json event_json;
    event_json["ts"] = Timestamp();
    event_json["event"] = "turn_end";
    event_json["turn"] = m_turn;
    event_json["session_id"] = m_session_id;
    event_json["run_id"] = m_run_id;
    event_json["ok"] = ok;
    event_json["error"] = Sanitize(error);
    event_json["steps"] = steps;

    if (event != nullptr)
    {
        event_json["status"] = event->status;
        event_json["error_code"] = event->error_code;
        event_json["tool_attempts"] = event->tool_attempts;
        event_json["replayed"] = event->replayed;
    }

    event_json["duration_ms"] = ElapsedMillis(m_turn_started);
    WriteEvent(event_json.dump());
}

void FileTracer::WriteEvent(const std::string& json_line)
{
    if (m_sink != nullptr)
    {
        m_sink->Write(json_line);
    }
}

std::string FileTracer::Timestamp() const
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string FileTracer::Sanitize(std::string text) const
{
    const std::string key_prefix = "sk-";
    std::size_t position = 0;

    // 脱敏形如 sk-XXXXXXXX 的 API key（主体至少 8 位），短串保持原样。
    while ((position = text.find(key_prefix, position)) != std::string::npos)
    {
        std::size_t end = position + key_prefix.size();
        while (end < text.size())
        {
            const unsigned char character = static_cast<unsigned char>(text[end]);
            const bool is_key_character = std::isalnum(character) != 0 || character == '_' || character == '-';
            if (!is_key_character)
            {
                break;
            }
            ++end;
        }
        if (end - position >= key_prefix.size() + 8)
        {
            text.replace(position, end - position, "[REDACTED]");
            position += 10;
        }
        else
        {
            position = end;
        }
    }

    if (text.size() > MAX_TRACE_TEXT)
    {
        text.resize(core::Utf8PrefixSize(text, MAX_TRACE_TEXT));
        text += "... [truncated]";
    }

    return text;
}

} // namespace agent::plugins::traces
