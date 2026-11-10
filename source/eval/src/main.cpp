#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/core/chat_model.h"
#include "agent/core/memory.h"
#include "agent/core/message.h"
#include "agent/core/system_registry.h"
#include "agent/model/openai_chat_model.h"
#include "agent/plugins/memory/in_memory_store.h"
#include "agent/plugins/tools/list_dir_tool.h"
#include "agent/plugins/tools/read_file_tool.h"
#include "agent/plugins/tools/search_text_tool.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/agent_profile.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/context_policy.h"
#include "agent/runtime/run_context.h"
#include "agent/runtime/tool_dispatcher.h"
#include "agent/runtime/tool_output_policy.h"
#include "agent/runtime/tool_policy.h"
#include "agent/runtime/tool_registry.h"
#include "agent/transport/winhttp_transport.h"

namespace agent::eval
{

namespace
{

constexpr const char* kMockMode = "mock";
constexpr const char* kRealMode = "real";
constexpr double kDefaultMinPassRate = 0.7;
constexpr int kDefaultMaxSteps = 8;
constexpr int kDefaultMaxTokens = 1024;
constexpr std::size_t kDefaultMaxToolCalls = 64;
constexpr std::size_t kDefaultMaxToolOutputBytes = 32 * 1024;
constexpr const char* kDefaultModel = "deepseek-flash";
constexpr const char* kDefaultSystemPrompt = "You are an offline evaluation agent. Answer concisely.";
constexpr const char* kReadFileToolName = "read_file";
constexpr const char* kListDirToolName = "list_dir";
constexpr const char* kSearchTextToolName = "search_text";
constexpr const char* kWorkspaceDirectoryName = "workspace";
constexpr const char* kApiKeyEnvironmentName = "DEEPSEEK_API_KEY";
constexpr const char* kAgentName = "agent-eval";
constexpr const char* kAgentVersion = "0.1.0";
constexpr const char* kDefaultMemoryScopeKind = "cli";
constexpr const char* kDefaultMemoryScopeId = "default";

/// CLI 解析后的运行配置。
struct CliOptions
{
    // 输入与模式
    std::filesystem::path tasks_path;
    bool has_tasks_path = false;
    std::string mode = kMockMode;

    // 判定与输出
    double min_pass_rate = kDefaultMinPassRate;
    std::optional<std::filesystem::path> report_path;

    // 工作区与调试
    std::optional<std::filesystem::path> workspace;
    bool verbose = false;
    bool help = false;
};

/// 单条任务的期望结果；未填写的可选字段不做校验。
struct TaskExpectation
{
    // 运行结果
    runtime::RunStatus status = runtime::RunStatus::Completed;
    runtime::RunErrorCode error_code = runtime::RunErrorCode::None;

    // 执行计数
    std::optional<int> min_steps;
    std::optional<int> max_steps;

    // 工具轨迹
    std::optional<std::vector<std::string>> tool_names;
    std::optional<std::vector<bool>> tool_results_ok;

    // 最终回答内容与召回注入
    std::optional<std::string> content_contains;
    std::optional<std::string> request_contains;
};

/// 一条评测任务：输入、记忆种子、执行预算、mock 脚本与期望。
struct EvalTask
{
    // 标识与说明
    std::string id;
    std::string kind;
    std::string description;

    // 输入与提示词
    std::string input;
    std::string system_prompt = kDefaultSystemPrompt;

    // 多轮输入：首次 input 之后依次对同一 Agent 继续 Send
    std::vector<std::string> follow_up_inputs;

    // 执行预算
    int max_steps = kDefaultMaxSteps;
    std::size_t max_tool_calls = kDefaultMaxToolCalls;
    std::size_t max_tool_output_bytes = kDefaultMaxToolOutputBytes;

    // 长期记忆种子：非空时写入 InMemoryMemoryStore 并启用召回
    std::vector<core::MemoryRecord> memory;

    // mock 模式脚本：按顺序返回的模型响应
    std::vector<core::ChatResponse> script;

    // 判定条件
    TaskExpectation expect;
};

/// 单条任务的执行与判定记录。
struct TaskResult
{
    // 标识与说明
    std::string id;
    std::string kind;
    std::string description;

    // 判定
    bool passed = false;
    std::string failure;

    // 运行结果
    std::string status;
    std::string error_code;
    std::string run_error;

    // 执行计数
    int steps = 0;
    std::size_t tool_attempts = 0;

    // 工具轨迹
    std::vector<std::string> tools;
    std::vector<bool> tool_results_ok;

    // 模型调用与 token 用量
    int model_calls = 0;
    std::int64_t tokens = 0;

    // 召回注入实测：是否满足 expect.request_contains（未要求时为 false）
    bool request_contains = false;

    // 耗时
    std::int64_t latency_ms = 0;
};

/// 一次执行尝试：setup_error 非空表示会话没有跑起来。
struct RunAttempt
{
    // 首轮执行结果
    runtime::AgentRunResult result;
    std::string setup_error;

    // 多轮复用：首次 Send 之后继续执行 follow_up_inputs
    std::shared_ptr<runtime::Agent> agent;
};

/// 任务 JSON 格式错误；LoadTasks 捕获后统一转成可读错误。
class TaskFormatError final : public std::runtime_error
{
  public:
    explicit TaskFormatError(const std::string& message)
        : std::runtime_error(message)
    {}
};

/// mock 模式模型：按脚本顺序返回 ChatResponse，脚本耗尽后返回 ModelFailure。
class ScriptedChatModel final : public core::ChatModel
{
  public:
    explicit ScriptedChatModel(std::vector<core::ChatResponse> responses)
        : m_responses(std::move(responses))
    {}

    core::ChatResponse Chat(const core::ChatRequest&) override
    {
        if (m_next >= m_responses.size())
        {
            core::ChatResponse response;
            response.ok = false;
            response.error = "mock script exhausted: no more scripted responses";
            response.error_code = core::CallError::ModelFailure;
            return response;
        }
        core::ChatResponse response = m_responses[m_next];
        ++m_next;
        return response;
    }

  private:
    std::vector<core::ChatResponse> m_responses;
    std::size_t m_next = 0;
};

/// 模型装饰器：累计 usage 与调用次数，供 token / model_calls 指标使用。
class TrackingChatModel final : public core::ChatModel
{
  public:
    explicit TrackingChatModel(std::shared_ptr<core::ChatModel> inner)
        : m_inner(std::move(inner))
    {}

    core::ChatResponse Chat(const core::ChatRequest& request) override
    {
        if (!m_inner)
        {
            core::ChatResponse response;
            response.ok = false;
            response.error = "chat model is not configured";
            response.error_code = core::CallError::ModelFailure;
            return response;
        }
        core::ChatResponse response = m_inner->Chat(request);
        ++m_calls;
        m_usage.prompt_tokens += response.usage.prompt_tokens;
        m_usage.completion_tokens += response.usage.completion_tokens;
        m_usage.total_tokens += response.usage.total_tokens;
        return response;
    }

    const core::ChatUsage& Usage() const
    {
        return m_usage;
    }

    int Calls() const
    {
        return m_calls;
    }

  private:
    std::shared_ptr<core::ChatModel> m_inner;
    core::ChatUsage m_usage;
    int m_calls = 0;
};

/// 模型装饰器：记录每次 ChatRequest 拷贝，用于验证召回内容是否真的注入了 prompt。
class RecordingChatModel final : public core::ChatModel
{
  public:
    explicit RecordingChatModel(std::shared_ptr<core::ChatModel> inner)
        : m_inner(std::move(inner))
    {}

    core::ChatResponse Chat(const core::ChatRequest& request) override
    {
        m_requests.push_back(request);
        if (!m_inner)
        {
            core::ChatResponse response;
            response.ok = false;
            response.error = "chat model is not configured";
            response.error_code = core::CallError::ModelFailure;
            return response;
        }
        return m_inner->Chat(request);
    }

    /// 整个任务期间是否有任一请求的任一 message.content 包含 text。
    bool ContainsText(const std::string& text) const
    {
        for (const core::ChatRequest& request : m_requests)
        {
            for (const core::ChatMessage& message : request.messages)
            {
                if (message.content.find(text) != std::string::npos)
                {
                    return true;
                }
            }
        }
        return false;
    }

  private:
    std::shared_ptr<core::ChatModel> m_inner;
    std::vector<core::ChatRequest> m_requests;
};

constexpr runtime::RunStatus kRunStatuses[] = {
    runtime::RunStatus::Completed,      runtime::RunStatus::Cancelled, runtime::RunStatus::Failed,
    runtime::RunStatus::BudgetExceeded, runtime::RunStatus::Busy,
};

constexpr runtime::RunErrorCode kRunErrorCodes[] = {
    runtime::RunErrorCode::None,
    runtime::RunErrorCode::Cancelled,
    runtime::RunErrorCode::DeadlineExceeded,
    runtime::RunErrorCode::InvalidInput,
    runtime::RunErrorCode::InvalidConfiguration,
    runtime::RunErrorCode::Busy,
    runtime::RunErrorCode::ModelCallLimit,
    runtime::RunErrorCode::ToolCallLimit,
    runtime::RunErrorCode::ToolOutputLimit,
    runtime::RunErrorCode::ModelFailure,
    runtime::RunErrorCode::IncompleteResponse,
    runtime::RunErrorCode::EmptyResponse,
    runtime::RunErrorCode::ToolDenied,
    runtime::RunErrorCode::ToolFailure,
    runtime::RunErrorCode::InvalidToolCall,
    runtime::RunErrorCode::ContextFailure,
    runtime::RunErrorCode::InternalError,
    runtime::RunErrorCode::ValidationFailed,
};

constexpr core::CallError kCallErrors[] = {
    core::CallError::None,         core::CallError::Cancelled,   core::CallError::DeadlineExceeded,
    core::CallError::ModelFailure, core::CallError::ToolFailure,
};

constexpr core::MemoryKind kMemoryKinds[] = {
    core::MemoryKind::Fact,       core::MemoryKind::Event,        core::MemoryKind::Episode,
    core::MemoryKind::Preference, core::MemoryKind::Relationship, core::MemoryKind::Goal,
};

std::string UsageText()
{
    return "usage: agent-eval --tasks <path> [--mode mock|real] [--min-pass-rate <ratio>] [--report <json>] "
           "[--workspace <dir>] [--verbose]\n"
           "\n"
           "  --tasks <path>           evaluation task file (JSON)\n"
           "  --mode mock|real         mock = scripted offline model (default); real = DeepSeek API\n"
           "  --min-pass-rate <ratio>  required pass ratio in [0, 1] (default: 0.70)\n"
           "  --report <json>          write a JSON report to this path\n"
           "  --workspace <dir>        fixture workspace (default: <tasks dir>/workspace)\n"
           "  --verbose                print extra per-task details\n";
}

std::string PathText(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string ReadEnvironmentVariable(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

std::string JoinStrings(const std::vector<std::string>& values, const std::string& separator)
{
    std::string text;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
        {
            text += separator;
        }
        text += values[index];
    }
    return text;
}

std::string FormatRatio(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

std::string FormatToolNames(const std::vector<std::string>& names)
{
    std::string text = "[";
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        if (index != 0)
        {
            text += ", ";
        }
        text += names[index];
    }
    text += "]";
    return text;
}

std::string FormatToolResults(const std::vector<bool>& values)
{
    std::string text = "[";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
        {
            text += ", ";
        }
        text += values[index] ? "true" : "false";
    }
    text += "]";
    return text;
}

bool ParseRunStatusName(const std::string& text, runtime::RunStatus& value)
{
    for (const runtime::RunStatus candidate : kRunStatuses)
    {
        if (text == runtime::ToString(candidate))
        {
            value = candidate;
            return true;
        }
    }
    return false;
}

bool ParseRunErrorCodeName(const std::string& text, runtime::RunErrorCode& value)
{
    for (const runtime::RunErrorCode candidate : kRunErrorCodes)
    {
        if (text == runtime::ToString(candidate))
        {
            value = candidate;
            return true;
        }
    }
    return false;
}

bool ParseCallErrorName(const std::string& text, core::CallError& value)
{
    for (const core::CallError candidate : kCallErrors)
    {
        if (text == core::ToString(candidate))
        {
            value = candidate;
            return true;
        }
    }
    return false;
}

bool ParseMemoryKindName(const std::string& text, core::MemoryKind& value)
{
    for (const core::MemoryKind candidate : kMemoryKinds)
    {
        if (text == core::ToString(candidate))
        {
            value = candidate;
            return true;
        }
    }
    return false;
}

bool IsSupportedMode(const std::string& mode)
{
    return mode == kMockMode || mode == kRealMode;
}

bool IsJsonInteger(const nlohmann::json& value)
{
    return value.is_number_integer() || value.is_number_unsigned();
}

bool HasInvertedStepRange(const std::optional<int>& min_steps, const std::optional<int>& max_steps)
{
    return min_steps && max_steps && *min_steps > *max_steps;
}

bool HasTasksArray(const nlohmann::json& document)
{
    return document.is_object() && document.contains("tasks") && document.at("tasks").is_array();
}

bool ParseRatio(const std::string& text, double& value, std::string& error)
{
    if (text.empty())
    {
        error = "must not be empty";
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == nullptr || end == text.c_str() || *end != '\0')
    {
        error = "must be a number: " + text;
        return false;
    }
    if (!(parsed >= 0.0 && parsed <= 1.0))
    {
        error = "must be within [0, 1]: " + text;
        return false;
    }
    value = parsed;
    return true;
}

std::string RequireString(const std::string& where, const nlohmann::json& object, const char* key)
{
    if (!object.contains(key))
    {
        throw TaskFormatError(where + "missing required field: " + key);
    }
    const nlohmann::json& value = object.at(key);
    if (!value.is_string())
    {
        throw TaskFormatError(where + "field must be a string: " + key);
    }
    return value.get<std::string>();
}

std::string ReadOptionalString(const std::string& where, const nlohmann::json& object, const char* key,
                               std::string fallback)
{
    if (!object.contains(key) || object.at(key).is_null())
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(key);
    if (!value.is_string())
    {
        throw TaskFormatError(where + "field must be a string: " + key);
    }
    return value.get<std::string>();
}

std::int64_t ReadOptionalInteger(const std::string& where, const nlohmann::json& object, const char* key,
                                 std::int64_t fallback)
{
    if (!object.contains(key) || object.at(key).is_null())
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(key);
    if (!IsJsonInteger(value))
    {
        throw TaskFormatError(where + "field must be an integer: " + key);
    }
    return value.get<std::int64_t>();
}

std::optional<int> ReadOptionalInt(const std::string& where, const nlohmann::json& object, const char* key)
{
    if (!object.contains(key) || object.at(key).is_null())
    {
        return std::nullopt;
    }
    const std::int64_t parsed = ReadOptionalInteger(where, object, key, 0);
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
    {
        throw TaskFormatError(where + "field is out of range: " + key);
    }
    return static_cast<int>(parsed);
}

std::size_t ReadOptionalSize(const std::string& where, const nlohmann::json& object, const char* key,
                             std::size_t fallback)
{
    if (!object.contains(key) || object.at(key).is_null())
    {
        return fallback;
    }
    const std::int64_t parsed = ReadOptionalInteger(where, object, key, 0);
    if (parsed < 0)
    {
        throw TaskFormatError(where + "field must not be negative: " + key);
    }
    return static_cast<std::size_t>(parsed);
}

bool ReadOptionalBool(const std::string& where, const nlohmann::json& object, const char* key, bool fallback)
{
    if (!object.contains(key) || object.at(key).is_null())
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(key);
    if (!value.is_boolean())
    {
        throw TaskFormatError(where + "field must be a boolean: " + key);
    }
    return value.get<bool>();
}

double ReadOptionalDouble(const std::string& where, const nlohmann::json& object, const char* key, double fallback)
{
    if (!object.contains(key) || object.at(key).is_null())
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(key);
    if (!value.is_number())
    {
        throw TaskFormatError(where + "field must be a number: " + key);
    }
    return value.get<double>();
}

std::vector<std::string> ReadStringArray(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_array())
    {
        throw TaskFormatError(where + "must be an array");
    }
    std::vector<std::string> result;
    result.reserve(value.size());
    std::size_t index = 0;
    for (const nlohmann::json& item : value)
    {
        if (!item.is_string())
        {
            throw TaskFormatError(where + "element #" + std::to_string(index) + " must be a string");
        }
        result.push_back(item.get<std::string>());
        ++index;
    }
    return result;
}

std::vector<bool> ReadBoolArray(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_array())
    {
        throw TaskFormatError(where + "must be an array");
    }
    std::vector<bool> result;
    result.reserve(value.size());
    std::size_t index = 0;
    for (const nlohmann::json& item : value)
    {
        if (!item.is_boolean())
        {
            throw TaskFormatError(where + "element #" + std::to_string(index) + " must be a boolean");
        }
        result.push_back(item.get<bool>());
        ++index;
    }
    return result;
}

core::MemoryScope ParseMemoryScope(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_object())
    {
        throw TaskFormatError(where + "must be an object");
    }
    core::MemoryScope scope;
    scope.kind = ReadOptionalString(where, value, "kind", kDefaultMemoryScopeKind);
    scope.id = ReadOptionalString(where, value, "id", kDefaultMemoryScopeId);
    return scope;
}

core::MemoryRecord ParseMemoryRecord(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_object())
    {
        throw TaskFormatError(where + "seed must be an object");
    }

    core::MemoryRecord record;
    record.id = RequireString(where, value, "id");
    const std::string kind_name = ReadOptionalString(where, value, "kind", core::ToString(core::MemoryKind::Fact));
    if (!ParseMemoryKindName(kind_name, record.kind))
    {
        throw TaskFormatError(where + "unknown memory kind: " + kind_name);
    }
    record.text = RequireString(where, value, "text");

    // 可选字段：标签、排序权重、作用域与时间
    if (value.contains("tags"))
    {
        record.tags = ReadStringArray(where + "tags: ", value.at("tags"));
    }
    record.importance = ReadOptionalDouble(where, value, "importance", record.importance);
    record.confidence = ReadOptionalDouble(where, value, "confidence", record.confidence);
    if (value.contains("scope"))
    {
        record.scope = ParseMemoryScope(where + "scope: ", value.at("scope"));
    }
    else
    {
        record.scope.kind = kDefaultMemoryScopeKind;
        record.scope.id = kDefaultMemoryScopeId;
    }
    record.payload_json = ReadOptionalString(where, value, "payload_json", "");
    record.created_at = ReadOptionalInteger(where, value, "created_at", 0);
    record.updated_at = ReadOptionalInteger(where, value, "updated_at", 0);
    return record;
}

std::vector<core::MemoryRecord> ParseMemorySeeds(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_array())
    {
        throw TaskFormatError(where + "must be an array");
    }
    std::vector<core::MemoryRecord> records;
    records.reserve(value.size());
    std::size_t index = 0;
    for (const nlohmann::json& item : value)
    {
        records.push_back(ParseMemoryRecord(where + "[" + std::to_string(index) + "]: ", item));
        ++index;
    }
    return records;
}

core::ChatUsage ParseUsage(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_object())
    {
        throw TaskFormatError(where + "usage must be an object");
    }
    core::ChatUsage usage;
    usage.prompt_tokens = ReadOptionalInteger(where, value, "prompt_tokens", 0);
    usage.completion_tokens = ReadOptionalInteger(where, value, "completion_tokens", 0);
    usage.total_tokens =
        ReadOptionalInteger(where, value, "total_tokens", usage.prompt_tokens + usage.completion_tokens);
    return usage;
}

std::vector<core::ToolCall> ParseToolCalls(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_array())
    {
        throw TaskFormatError(where + "tool_calls must be an array");
    }

    std::vector<core::ToolCall> calls;
    calls.reserve(value.size());
    std::size_t index = 0;

    for (const nlohmann::json& item : value)
    {
        const std::string call_where = where + "[" + std::to_string(index) + "]: ";
        if (!item.is_object())
        {
            throw TaskFormatError(call_where + "tool call must be an object");
        }
        core::ToolCall call;
        call.id = RequireString(call_where, item, "id");
        call.name = RequireString(call_where, item, "name");
        if (!item.contains("arguments"))
        {
            throw TaskFormatError(call_where + "missing required field: arguments");
        }
        const nlohmann::json& arguments = item.at("arguments");
        if (arguments.is_string())
        {
            call.arguments_json = arguments.get<std::string>();
        }
        else if (arguments.is_object() || arguments.is_array())
        {
            call.arguments_json = arguments.dump();
        }
        else
        {
            throw TaskFormatError(call_where + "arguments must be a string or a JSON object");
        }
        calls.push_back(std::move(call));
        ++index;
    }
    return calls;
}

core::ChatResponse ParseScriptResponse(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_object())
    {
        throw TaskFormatError(where + "response must be an object");
    }

    core::ChatResponse response;
    response.ok = ReadOptionalBool(where, value, "ok", true);
    response.content = ReadOptionalString(where, value, "content", "");
    response.finish_reason = ReadOptionalString(where, value, "finish_reason", "stop");
    response.error = ReadOptionalString(where, value, "error", "");
    const std::string error_code_name =
        ReadOptionalString(where, value, "error_code", core::ToString(core::CallError::None));
    if (!ParseCallErrorName(error_code_name, response.error_code))
    {
        throw TaskFormatError(where + "unknown error_code: " + error_code_name);
    }

    // 可选字段：用量与工具调用
    if (value.contains("usage"))
    {
        response.usage = ParseUsage(where + "usage: ", value.at("usage"));
    }
    if (value.contains("tool_calls"))
    {
        response.tool_calls = ParseToolCalls(where + "tool_calls: ", value.at("tool_calls"));
    }
    return response;
}

std::vector<core::ChatResponse> ParseScript(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_array())
    {
        throw TaskFormatError(where + "script must be an array");
    }
    std::vector<core::ChatResponse> responses;
    responses.reserve(value.size());
    std::size_t index = 0;
    for (const nlohmann::json& item : value)
    {
        responses.push_back(ParseScriptResponse(where + "[" + std::to_string(index) + "]: ", item));
        ++index;
    }
    return responses;
}

TaskExpectation ParseExpectation(const std::string& where, const nlohmann::json& value)
{
    if (!value.is_object())
    {
        throw TaskFormatError(where + "expect must be an object");
    }

    TaskExpectation expectation;
    const std::string status_name = RequireString(where, value, "status");
    if (!ParseRunStatusName(status_name, expectation.status))
    {
        throw TaskFormatError(where + "unknown status: " + status_name);
    }
    const std::string error_code_name = RequireString(where, value, "error_code");
    if (!ParseRunErrorCodeName(error_code_name, expectation.error_code))
    {
        throw TaskFormatError(where + "unknown error_code: " + error_code_name);
    }

    // 可选约束：步数范围、工具轨迹、最终回答与召回注入
    expectation.min_steps = ReadOptionalInt(where, value, "min_steps");
    expectation.max_steps = ReadOptionalInt(where, value, "max_steps");
    if (HasInvertedStepRange(expectation.min_steps, expectation.max_steps))
    {
        throw TaskFormatError(where + "min_steps must not exceed max_steps");
    }
    if (value.contains("tools"))
    {
        expectation.tool_names = ReadStringArray(where + "tools: ", value.at("tools"));
    }
    if (value.contains("tool_results_ok"))
    {
        expectation.tool_results_ok = ReadBoolArray(where + "tool_results_ok: ", value.at("tool_results_ok"));
    }
    if (value.contains("content_contains"))
    {
        expectation.content_contains = RequireString(where, value, "content_contains");
    }
    if (value.contains("request_contains"))
    {
        expectation.request_contains = RequireString(where, value, "request_contains");
    }
    return expectation;
}

void ParseTask(const nlohmann::json& value, std::size_t index, EvalTask& task)
{
    const std::string position = "task #" + std::to_string(index);
    if (!value.is_object())
    {
        throw TaskFormatError(position + " must be an object");
    }

    task.id = RequireString(position + ": ", value, "id");
    const std::string where = "task \"" + task.id + "\": ";
    task.kind = RequireString(where, value, "kind");
    task.description = ReadOptionalString(where, value, "description", "");
    task.input = RequireString(where, value, "input");
    if (value.contains("follow_up_inputs"))
    {
        task.follow_up_inputs = ReadStringArray(where + "follow_up_inputs: ", value.at("follow_up_inputs"));
    }
    task.system_prompt = ReadOptionalString(where, value, "system_prompt", kDefaultSystemPrompt);

    // 执行预算：max_steps 必须为正
    task.max_steps = ReadOptionalInt(where, value, "max_steps").value_or(kDefaultMaxSteps);
    if (task.max_steps <= 0)
    {
        throw TaskFormatError(where + "max_steps must be positive");
    }
    task.max_tool_calls = ReadOptionalSize(where, value, "max_tool_calls", kDefaultMaxToolCalls);
    task.max_tool_output_bytes = ReadOptionalSize(where, value, "max_tool_output_bytes", kDefaultMaxToolOutputBytes);

    // 长期记忆种子、mock 脚本（可选）与期望（必填）
    if (value.contains("memory"))
    {
        task.memory = ParseMemorySeeds(where + "memory: ", value.at("memory"));
    }
    if (value.contains("script"))
    {
        task.script = ParseScript(where + "script", value.at("script"));
    }
    if (!value.contains("expect"))
    {
        throw TaskFormatError(where + "missing required field: expect");
    }
    task.expect = ParseExpectation(where, value.at("expect"));
}

const nlohmann::json* FindTaskList(const nlohmann::json& document)
{
    if (document.is_array())
    {
        return &document;
    }
    if (HasTasksArray(document))
    {
        return &document.at("tasks");
    }
    return nullptr;
}

bool ReadTextFile(const std::filesystem::path& path, std::string& text, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "cannot open file: " + PathText(path);
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad())
    {
        error = "cannot read file: " + PathText(path);
        return false;
    }
    text = buffer.str();
    return true;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text, std::string& error)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        error = "cannot open file for writing: " + PathText(path);
        return false;
    }
    file << text;
    if (!file.good())
    {
        error = "cannot write file: " + PathText(path);
        return false;
    }
    return true;
}

bool LoadTasks(const std::filesystem::path& path, std::vector<EvalTask>& tasks, std::string& error)
{
    std::string text;
    if (!ReadTextFile(path, text, error))
    {
        return false;
    }

    // 解析 JSON 并定位任务数组
    nlohmann::json document;
    try
    {
        document = nlohmann::json::parse(text);
    }
    catch (const nlohmann::json::exception& parse_error)
    {
        error = "invalid JSON in " + PathText(path) + ": " + parse_error.what();
        return false;
    }
    const nlohmann::json* task_list = FindTaskList(document);
    if (task_list == nullptr)
    {
        error = "tasks file must be a JSON array or an object with a \"tasks\" array: " + PathText(path);
        return false;
    }

    tasks.clear();
    std::size_t index = 0;
    for (const nlohmann::json& item : *task_list)
    {
        EvalTask task;
        try
        {
            ParseTask(item, index, task);
        }
        catch (const TaskFormatError& task_error)
        {
            error = task_error.what();
            return false;
        }
        tasks.push_back(std::move(task));
        ++index;
    }

    // 后置校验：任务非空且 id 唯一
    if (tasks.empty())
    {
        error = "no tasks found in " + PathText(path);
        return false;
    }
    std::set<std::string> seen_ids;
    for (const EvalTask& task : tasks)
    {
        if (!seen_ids.insert(task.id).second)
        {
            error = "duplicate task id: " + task.id;
            return false;
        }
    }
    return true;
}

bool ResolveWorkspace(const CliOptions& options, const std::filesystem::path& tasks_path,
                      std::filesystem::path& workspace, std::string& error)
{
    std::filesystem::path candidate =
        options.workspace ? *options.workspace : tasks_path.parent_path() / kWorkspaceDirectoryName;
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(candidate, filesystem_error) || filesystem_error)
    {
        error = "workspace directory not found: " + PathText(candidate);
        return false;
    }
    workspace = std::filesystem::weakly_canonical(candidate, filesystem_error);
    if (filesystem_error)
    {
        error = "cannot resolve workspace directory: " + PathText(candidate);
        return false;
    }
    return true;
}

bool ConsumeOptionValue(const std::vector<std::string>& arguments, std::size_t& index, std::string& value,
                        std::string& error)
{
    if (index + 1 >= arguments.size())
    {
        error = "missing value for option " + arguments[index];
        return false;
    }
    ++index;
    value = arguments[index];
    return true;
}

bool ParseCliOptions(const std::vector<std::string>& arguments, CliOptions& options, std::string& error)
{
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string& argument = arguments[index];
        std::string value;
        if (argument == "--help" || argument == "-h")
        {
            options.help = true;
        }
        else if (argument == "--verbose" || argument == "-v")
        {
            options.verbose = true;
        }
        else if (argument == "--tasks")
        {
            if (!ConsumeOptionValue(arguments, index, value, error))
            {
                return false;
            }
            options.tasks_path = std::filesystem::path(value);
            options.has_tasks_path = true;
        }
        else if (argument == "--mode")
        {
            if (!ConsumeOptionValue(arguments, index, value, error))
            {
                return false;
            }
            if (!IsSupportedMode(value))
            {
                error = "unsupported mode: " + value + " (expected mock or real)";
                return false;
            }
            options.mode = value;
        }
        else if (argument == "--min-pass-rate")
        {
            if (!ConsumeOptionValue(arguments, index, value, error))
            {
                return false;
            }
            std::string ratio_error;
            if (!ParseRatio(value, options.min_pass_rate, ratio_error))
            {
                error = "--min-pass-rate " + ratio_error;
                return false;
            }
        }
        else if (argument == "--report")
        {
            if (!ConsumeOptionValue(arguments, index, value, error))
            {
                return false;
            }
            options.report_path = std::filesystem::path(value);
        }
        else if (argument == "--workspace")
        {
            if (!ConsumeOptionValue(arguments, index, value, error))
            {
                return false;
            }
            options.workspace = std::filesystem::path(value);
        }
        else
        {
            error = "unknown argument: " + argument;
            return false;
        }
    }

    // 循环结束后校验必填项；--help 优先返回
    if (options.help)
    {
        return true;
    }
    if (!options.has_tasks_path)
    {
        error = "missing required option: --tasks <path>";
        return false;
    }
    return true;
}

runtime::AgentProfile MakeTaskProfile(const EvalTask& task)
{
    runtime::AgentProfile profile;
    profile.model = kDefaultModel;
    profile.system_prompt = task.system_prompt;
    profile.tool_names = {kReadFileToolName, kListDirToolName, kSearchTextToolName};
    profile.temperature = 0.0;
    profile.max_tokens = kDefaultMaxTokens;
    profile.max_steps = task.max_steps;
    profile.max_tool_calls = task.max_tool_calls;
    profile.max_tool_output_bytes = task.max_tool_output_bytes;

    // 长期记忆：非空种子时启用召回；recall_scopes 留空 = 不限制 scope
    if (!task.memory.empty())
    {
        profile.memory_policy.enable_recall = true;
    }
    return profile;
}

std::shared_ptr<TrackingChatModel> CreateTaskModel(const EvalTask& task, const std::string& mode,
                                                   const std::shared_ptr<core::ChatModel>& real_model)
{
    if (mode == kRealMode)
    {
        return std::make_shared<TrackingChatModel>(real_model);
    }
    return std::make_shared<TrackingChatModel>(std::make_shared<ScriptedChatModel>(task.script));
}

/// 把任务里的 seed 记忆写入 InMemoryMemoryStore，并挂到 dependencies；无种子时不启用。
bool SeedMemory(const EvalTask& task, runtime::RunnerDependencies& dependencies, std::string& error)
{
    if (task.memory.empty())
    {
        return true;
    }
    auto store = std::make_shared<plugins::memory::InMemoryMemoryStore>();
    for (const core::MemoryRecord& record : task.memory)
    {
        std::string remember_error;
        if (!store->Remember(record, remember_error))
        {
            error = "cannot seed memory record \"" + record.id + "\": " + remember_error;
            return false;
        }
    }
    dependencies.memory = std::move(store);
    return true;
}

RunAttempt ExecuteTask(const EvalTask& task, const std::filesystem::path& workspace,
                       const std::shared_ptr<core::ChatModel>& model)
{
    RunAttempt attempt;
    auto tools = std::make_shared<runtime::ToolRegistry>();
    auto read_file_tool = std::make_shared<plugins::tools::ReadFileTool>(workspace);
    auto list_dir_tool = std::make_shared<plugins::tools::ListDirTool>(workspace);
    auto search_text_tool = std::make_shared<plugins::tools::SearchTextTool>(workspace);
    std::string register_error;
    if (!tools->Register(read_file_tool, register_error))
    {
        attempt.setup_error = "cannot register read_file tool: " + register_error;
        return attempt;
    }
    if (!tools->Register(list_dir_tool, register_error))
    {
        attempt.setup_error = "cannot register list_dir tool: " + register_error;
        return attempt;
    }
    if (!tools->Register(search_text_tool, register_error))
    {
        attempt.setup_error = "cannot register search_text tool: " + register_error;
        return attempt;
    }

    const runtime::AgentProfile profile = MakeTaskProfile(task);
    runtime::RunnerDependencies dependencies;
    dependencies.model = model;
    dependencies.tools = tools;
    dependencies.tool_policy = std::make_shared<runtime::AllowListToolPolicy>(profile.tool_names);
    dependencies.context_policy = std::make_shared<runtime::KeepRecentTurnsPolicy>();
    dependencies.tool_output_policy = std::make_shared<runtime::TruncateToolOutputPolicy>();
    dependencies.tool_scheduler = std::make_shared<runtime::InlineToolScheduler>();
    dependencies.systems = std::make_shared<core::SystemRegistry>();

    // 长期记忆：先写入 seed 记录；失败则本次尝试不启动会话
    std::string memory_error;
    if (!SeedMemory(task, dependencies, memory_error))
    {
        attempt.setup_error = memory_error;
        return attempt;
    }

    try
    {
        auto runner = std::make_shared<runtime::AgentRunner>(std::move(dependencies));
        runtime::AgentDescriptor descriptor;
        descriptor.id.value = task.id;
        descriptor.name = kAgentName;
        descriptor.version = kAgentVersion;
        descriptor.capabilities = {kReadFileToolName, kListDirToolName, kSearchTextToolName};
        runtime::AgentDependencies agent_dependencies;
        agent_dependencies.executor = runner;
        attempt.agent = std::make_shared<runtime::Agent>(descriptor, profile, agent_dependencies, "eval-" + task.id);
        runtime::RunOptions run_options;
        run_options.run_id = task.id;
        attempt.result = attempt.agent->Send(task.input, run_options);
    }
    catch (const std::exception& error)
    {
        attempt.setup_error = error.what();
    }
    catch (...)
    {
        attempt.setup_error = "unknown exception";
    }
    return attempt;
}

void FillRunResult(TaskResult& result, const runtime::AgentRunResult& run)
{
    result.status = runtime::ToString(run.status);
    result.error_code = runtime::ToString(run.error_code);
    result.run_error = run.error;
    result.steps = run.steps;
    result.tool_attempts = run.tool_attempts;
    result.tools.clear();
    result.tool_results_ok.clear();
    for (const runtime::ToolExecutionRecord& execution : run.tool_executions)
    {
        result.tools.push_back(execution.call.name);
        result.tool_results_ok.push_back(execution.result.ok);
    }
}

std::string EvaluateTaskExpectation(const TaskExpectation& expectation, const TaskResult& result,
                                    const runtime::AgentRunResult& run)
{
    std::vector<std::string> failures;

    // 运行状态与执行计数
    const std::string expected_status = runtime::ToString(expectation.status);
    if (result.status != expected_status)
    {
        failures.push_back("status expected " + expected_status + ", got " + result.status);
    }
    const std::string expected_error_code = runtime::ToString(expectation.error_code);
    if (result.error_code != expected_error_code)
    {
        failures.push_back("error_code expected " + expected_error_code + ", got " + result.error_code);
    }
    if (expectation.min_steps && result.steps < *expectation.min_steps)
    {
        failures.push_back("steps expected >= " + std::to_string(*expectation.min_steps) + ", got " +
                           std::to_string(result.steps));
    }
    if (expectation.max_steps && result.steps > *expectation.max_steps)
    {
        failures.push_back("steps expected <= " + std::to_string(*expectation.max_steps) + ", got " +
                           std::to_string(result.steps));
    }

    // 工具轨迹、最终回答与召回注入
    if (expectation.tool_names && result.tools != *expectation.tool_names)
    {
        failures.push_back("tools expected " + FormatToolNames(*expectation.tool_names) + ", got " +
                           FormatToolNames(result.tools));
    }
    if (expectation.tool_results_ok && result.tool_results_ok != *expectation.tool_results_ok)
    {
        failures.push_back("tool_results_ok expected " + FormatToolResults(*expectation.tool_results_ok) + ", got " +
                           FormatToolResults(result.tool_results_ok));
    }
    if (expectation.content_contains && run.content.find(*expectation.content_contains) == std::string::npos)
    {
        failures.push_back("content does not contain: " + *expectation.content_contains);
    }
    if (expectation.request_contains && !result.request_contains)
    {
        failures.push_back("no model request contains: " + *expectation.request_contains);
    }
    return JoinStrings(failures, "; ");
}

/// 首轮成功后在同一个 Agent 上继续 follow_up_inputs；任一失败即停止，保留最后一轮结果。
void RunFollowUpTurns(const EvalTask& task, RunAttempt& attempt)
{
    if (!attempt.agent || !attempt.result.Ok())
    {
        return;
    }
    for (std::size_t index = 0; index < task.follow_up_inputs.size(); ++index)
    {
        runtime::RunOptions options;
        options.run_id = task.id + "-turn-" + std::to_string(index + 2);
        attempt.result = attempt.agent->Send(task.follow_up_inputs[index], options);
        if (!attempt.result.Ok())
        {
            break;
        }
    }
}

TaskResult RunTask(const EvalTask& task, const std::filesystem::path& workspace, const std::string& mode,
                   const std::shared_ptr<core::ChatModel>& real_model)
{
    TaskResult result;
    result.id = task.id;
    result.kind = task.kind;
    result.description = task.description;

    const auto tracker = CreateTaskModel(task, mode, real_model);
    const auto recorder = std::make_shared<RecordingChatModel>(tracker);
    const auto started = std::chrono::steady_clock::now();
    RunAttempt attempt = ExecuteTask(task, workspace, recorder);
    if (attempt.setup_error.empty())
    {
        RunFollowUpTurns(task, attempt);
    }

    const auto finished = std::chrono::steady_clock::now();
    result.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
    result.model_calls = tracker->Calls();
    result.tokens = tracker->Usage().total_tokens;
    if (task.expect.request_contains)
    {
        result.request_contains = recorder->ContainsText(*task.expect.request_contains);
    }

    if (!attempt.setup_error.empty())
    {
        result.status = "Failed";
        result.error_code = "InternalError";
        result.run_error = attempt.setup_error;
        result.failure = "runtime setup failed: " + attempt.setup_error;
        return result;
    }
    FillRunResult(result, attempt.result);
    result.failure = EvaluateTaskExpectation(task.expect, result, attempt.result);
    result.passed = result.failure.empty();
    return result;
}

void PrintTaskLine(const TaskResult& result, bool verbose)
{
    std::cout << (result.passed ? "[PASS] " : "[FAIL] ") << result.id << " kind=" << result.kind
              << " steps=" << result.steps << " tools=" << FormatToolNames(result.tools)
              << " latency_ms=" << result.latency_ms << " tokens=" << result.tokens;
    if (!result.passed)
    {
        std::cout << " failure=" << result.failure;
    }
    std::cout << "\n";
    if (!verbose)
    {
        return;
    }
    std::cout << "       status=" << result.status << " error_code=" << result.error_code
              << " tool_attempts=" << result.tool_attempts << " model_calls=" << result.model_calls
              << " tool_results_ok=" << FormatToolResults(result.tool_results_ok)
              << " request_contains=" << (result.request_contains ? "true" : "false");
    if (!result.run_error.empty())
    {
        std::cout << " run_error=" << result.run_error;
    }
    std::cout << "\n";
}

void PrintSummary(int passed, int total, double pass_rate, double threshold)
{
    std::cout << "summary: passed=" << passed << " total=" << total << " pass_rate=" << FormatRatio(pass_rate)
              << " threshold=" << FormatRatio(threshold) << "\n";
}

nlohmann::json BuildReportJson(const CliOptions& options, const std::filesystem::path& tasks_file,
                               const std::filesystem::path& workspace, const std::vector<TaskResult>& results,
                               int passed, double pass_rate)
{
    nlohmann::json report;
    report["tool"] = "agent-eval";
    report["mode"] = options.mode;
    report["tasks_file"] = PathText(tasks_file);
    report["workspace"] = PathText(workspace);
    report["passed"] = passed;
    report["total"] = static_cast<int>(results.size());
    report["pass_rate"] = pass_rate;
    report["threshold"] = options.min_pass_rate;

    // 逐条任务的明细结果
    nlohmann::json task_results = nlohmann::json::array();
    for (const TaskResult& result : results)
    {
        nlohmann::json item;
        item["id"] = result.id;
        item["kind"] = result.kind;
        item["description"] = result.description;
        item["passed"] = result.passed;
        item["failure"] = result.failure;
        item["status"] = result.status;
        item["error_code"] = result.error_code;
        item["run_error"] = result.run_error;
        item["steps"] = result.steps;
        item["tool_attempts"] = static_cast<std::uint64_t>(result.tool_attempts);
        item["tools"] = result.tools;
        nlohmann::json tool_results = nlohmann::json::array();
        for (const bool ok : result.tool_results_ok)
        {
            tool_results.push_back(ok);
        }
        item["tool_results_ok"] = std::move(tool_results);
        item["model_calls"] = result.model_calls;
        item["tokens"] = result.tokens;
        item["request_contains"] = result.request_contains;
        item["latency_ms"] = result.latency_ms;
        task_results.push_back(std::move(item));
    }
    report["results"] = std::move(task_results);
    return report;
}

} // namespace

int RunCli(const std::vector<std::string>& arguments)
{
    CliOptions options;
    std::string error;
    if (!ParseCliOptions(arguments, options, error))
    {
        std::cerr << "error: " << error << "\n\n" << UsageText();
        return 2;
    }
    if (options.help)
    {
        std::cout << UsageText();
        return 0;
    }

    std::filesystem::path tasks_file = options.tasks_path;
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(tasks_file, filesystem_error) || filesystem_error)
    {
        std::cerr << "error: tasks file not found: " << PathText(tasks_file) << "\n";
        return 2;
    }
    tasks_file = std::filesystem::weakly_canonical(tasks_file, filesystem_error);
    if (filesystem_error)
    {
        std::cerr << "error: cannot resolve tasks file: " << PathText(options.tasks_path) << "\n";
        return 2;
    }

    std::vector<EvalTask> tasks;
    if (!LoadTasks(tasks_file, tasks, error))
    {
        std::cerr << "error: " << error << "\n";
        return 2;
    }

    std::filesystem::path workspace;
    if (!ResolveWorkspace(options, tasks_file, workspace, error))
    {
        std::cerr << "error: " << error << "\n";
        return 2;
    }

    std::shared_ptr<core::ChatModel> real_model;
    if (options.mode == kRealMode)
    {
        const std::string api_key = ReadEnvironmentVariable(kApiKeyEnvironmentName);
        if (api_key.empty())
        {
            std::cerr << "error: real mode requires the " << kApiKeyEnvironmentName << " environment variable\n";
            return 2;
        }
#if defined(_WIN32)
        model::OpenAiConfig config;
        config.api_key = api_key;
        real_model = std::make_shared<model::OpenAiChatModel>(std::make_shared<transport::WinHttpTransport>(),
                                                              std::move(config));
#else
        std::cerr << "error: real mode requires the WinHTTP transport, which is available on Windows only\n";
        return 2;
#endif
    }

    std::cout << "agent-eval: mode=" << options.mode << " tasks=" << tasks.size()
              << " workspace=" << PathText(workspace) << "\n";

    std::vector<TaskResult> results;
    results.reserve(tasks.size());
    int passed = 0;
    for (const EvalTask& task : tasks)
    {
        TaskResult result = RunTask(task, workspace, options.mode, real_model);
        if (result.passed)
        {
            ++passed;
        }
        PrintTaskLine(result, options.verbose);
        results.push_back(std::move(result));
    }

    const int total = static_cast<int>(results.size());
    const double pass_rate = static_cast<double>(passed) / static_cast<double>(total);
    PrintSummary(passed, total, pass_rate, options.min_pass_rate);

    if (options.report_path)
    {
        const nlohmann::json report = BuildReportJson(options, tasks_file, workspace, results, passed, pass_rate);
        const std::string report_text = report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        if (!WriteTextFile(*options.report_path, report_text, error))
        {
            std::cerr << "error: " << error << "\n";
            return 2;
        }
        std::cout << "report: " << PathText(*options.report_path) << "\n";
    }

    const bool meets_threshold = pass_rate + 1e-9 >= options.min_pass_rate;
    return meets_threshold ? 0 : 1;
}

} // namespace agent::eval

int main(int argc, char** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }

    try
    {
        return agent::eval::RunCli(arguments);
    }
    catch (const std::exception& error)
    {
        std::cerr << "fatal: " << error.what() << "\n";
        return 2;
    }
    catch (...)
    {
        std::cerr << "fatal: unknown error\n";
        return 2;
    }
}
