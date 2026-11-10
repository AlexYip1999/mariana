#include "agent/plugins/tools/search_text_tool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent::plugins::tools
{

namespace
{

core::ToolResult MakeError(std::string message)
{
    core::ToolResult result;
    result.ok = false;
    result.error = std::move(message);
    return result;
}

std::filesystem::path Utf8Path(const std::string& text)
{
#if defined(_WIN32)
    const std::u8string utf8(reinterpret_cast<const char8_t*>(text.data()), text.size());
    return std::filesystem::path(utf8);
#else
    return std::filesystem::path(text);
#endif
}

std::string Utf8String(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

bool TryUtf8String(const std::filesystem::path& path, std::string& text)
{
    try
    {
        text = Utf8String(path);
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

bool IsSearchTextArgument(const std::string& key)
{
    return key == "query" || key == "path" || key == "max_results";
}

bool IsNegativeJsonInteger(const nlohmann::json& value)
{
    return !value.is_number_unsigned() && value.get<std::int64_t>() < 0;
}

struct ScanTarget
{
    std::filesystem::path path;
    std::string relative;
};

/// 解析并校验 workspace 相对路径；拒绝绝对路径、越界路径与敏感路径。
bool ResolveWorkspacePath(const WorkspacePathPolicy& policy, const std::string& relative_path,
                          std::filesystem::path& resolved, std::string& error)
{
    error.clear();
    if (relative_path.empty())
    {
        error = "path must not be empty";
        return false;
    }

    std::filesystem::path relative;
    try
    {
        relative = Utf8Path(relative_path);
    }
    catch (const std::exception&)
    {
        error = "invalid UTF-8 path";
        return false;
    }

    if (relative.is_absolute() || relative.has_root_name())
    {
        error = "absolute paths are not allowed";
        return false;
    }
    if (!policy.IsValid())
    {
        error = "cannot resolve workspace";
        return false;
    }

    std::error_code resolve_error;
    resolved = std::filesystem::weakly_canonical(policy.Root() / relative, resolve_error);
    if (resolve_error)
    {
        error = "cannot resolve path: " + relative_path;
        return false;
    }

    // workspace root 自身的 lexically_relative 为空，需单独放行。
    if (resolved != policy.Root() && !policy.IsWithinWorkspace(resolved))
    {
        error = "path escapes workspace";
        return false;
    }
    if (policy.IsSensitive(resolved.lexically_relative(policy.Root())))
    {
        error = "sensitive path is not readable";
        return false;
    }
    return true;
}

bool CollectTargets(const std::filesystem::path& start, const WorkspacePathPolicy& policy,
                    std::vector<ScanTarget>& targets, std::string& error)
{
    targets.clear();
    std::error_code status_error;
    const std::filesystem::file_status start_status = std::filesystem::status(start, status_error);
    if (status_error)
    {
        // 不存在的路径也可能以 error_code 报告（如 Windows 的 no_such_file_or_directory）。
        if (status_error == std::errc::no_such_file_or_directory)
        {
            error = "path not found";
            return false;
        }
        error = "cannot inspect path";
        return false;
    }
    if (start_status.type() == std::filesystem::file_type::not_found)
    {
        error = "path not found";
        return false;
    }

    if (std::filesystem::is_regular_file(start_status))
    {
        std::string relative;
        if (!TryUtf8String(start.lexically_relative(policy.Root()), relative))
        {
            error = "invalid UTF-8 path";
            return false;
        }
        targets.push_back({start, std::move(relative)});
        return true;
    }
    if (!std::filesystem::is_directory(start_status))
    {
        error = "path is not a file or directory";
        return false;
    }

    try
    {
        const std::filesystem::directory_options options = std::filesystem::directory_options::skip_permission_denied;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(start, options))
        {
            std::error_code entry_error;
            const std::filesystem::file_status entry_status = entry.symlink_status(entry_error);
            if (entry_error || entry_status.type() == std::filesystem::file_type::not_found)
            {
                continue;
            }
            if (std::filesystem::is_symlink(entry_status) || std::filesystem::is_directory(entry_status))
            {
                continue;
            }
            if (!std::filesystem::is_regular_file(entry_status))
            {
                continue;
            }

            const std::filesystem::path relative = entry.path().lexically_relative(policy.Root());
            if (policy.IsSensitive(relative))
            {
                continue;
            }

            // 复核 canonical 路径，避免经 junction 等重解析点读到 workspace 之外。
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(entry.path(), entry_error);
            if (entry_error || !policy.IsWithinWorkspace(canonical))
            {
                continue;
            }

            std::string relative_text;
            if (!TryUtf8String(relative, relative_text))
            {
                continue;
            }
            targets.push_back({entry.path(), std::move(relative_text)});
        }
    }
    catch (const std::exception&)
    {
        error = "cannot scan directory";
        return false;
    }

    std::sort(targets.begin(), targets.end(),
              [](const ScanTarget& left, const ScanTarget& right)
              {
                  return left.relative < right.relative;
              });
    return true;
}

/// 扫描单个文件；返回 false 表示命中数已达 max_results，应停止后续扫描。
bool ScanFile(const ScanTarget& target, const std::string& query, std::size_t max_file_bytes, std::size_t max_results,
              std::vector<std::string>& matches)
{
    std::error_code size_error;
    const std::uintmax_t file_size = std::filesystem::file_size(target.path, size_error);
    if (size_error || file_size > max_file_bytes)
    {
        return true;
    }

    std::ifstream file(target.path, std::ios::binary);
    if (!file)
    {
        return true;
    }

    std::string content(static_cast<std::size_t>(max_file_bytes) + 1, '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    const std::streamsize read_count = file.gcount();
    if (read_count < 0 || static_cast<std::uintmax_t>(read_count) > max_file_bytes)
    {
        return true;
    }
    content.resize(static_cast<std::size_t>(read_count));
    if (content.find('\0') != std::string::npos)
    {
        // 含 NUL 视为二进制文件，跳过。
        return true;
    }

    const std::string prefix = target.relative + ":";
    std::size_t line_start = 0;
    std::size_t line_number = 0;
    while (line_start <= content.size())
    {
        const std::size_t newline = content.find('\n', line_start);
        const std::size_t line_end = newline == std::string::npos ? content.size() : newline;
        std::string line = content.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        ++line_number;
        if (line.find(query) != std::string::npos)
        {
            if (matches.size() >= max_results)
            {
                return false;
            }
            matches.push_back(prefix + std::to_string(line_number) + ": " + line);
        }
        if (newline == std::string::npos)
        {
            break;
        }
        line_start = newline + 1;
        if (line_start == content.size())
        {
            break;
        }
    }
    return true;
}

std::string BuildContent(const std::vector<std::string>& matches, bool results_truncated, bool scan_truncated)
{
    std::string content;
    for (std::size_t index = 0; index < matches.size(); ++index)
    {
        if (index != 0)
        {
            content.push_back('\n');
        }
        content += matches[index];
    }

    const char* note = nullptr;
    if (results_truncated)
    {
        note = "... (results truncated)";
    }
    else if (scan_truncated)
    {
        note = "... (scan truncated)";
    }
    if (note != nullptr)
    {
        if (!content.empty())
        {
            content.push_back('\n');
        }
        content += note;
    }
    return content;
}

} // namespace

SearchTextTool::SearchTextTool(std::filesystem::path workspace_root, std::size_t max_file_bytes,
                               std::size_t max_results, std::size_t max_files)
    : m_policy(std::move(workspace_root))
    , m_max_file_bytes(max_file_bytes)
    , m_max_results(max_results)
    , m_max_files(max_files)
{}

std::string SearchTextTool::Name() const
{
    return "search_text";
}

std::string SearchTextTool::Description() const
{
    return "Search text files inside the workspace.";
}

std::string SearchTextTool::ParametersJson() const
{
    return R"({"type":"object","properties":{"query":{"type":"string","description":"Case-sensitive substring to search for."},"path":{"type":"string","description":"Workspace-relative file or directory; defaults to the workspace root."},"max_results":{"type":"integer","minimum":1,"description":"Maximum matched lines to return; defaults to 50."}},"required":["query"],"additionalProperties":false})";
}

core::ToolResult SearchTextTool::Execute(const std::string& arguments_json, const core::CallContext& context)
{
    if (context.Interruption() != core::CallError::None)
    {
        auto result = MakeError("search_text interrupted");
        result.error_code = context.Interruption();
        return result;
    }

    nlohmann::json arguments;
    try
    {
        arguments = nlohmann::json::parse(arguments_json);
    }
    catch (const nlohmann::json::exception& error)
    {
        return MakeError(std::string("invalid arguments JSON: ") + error.what());
    }

    if (!arguments.is_object())
    {
        return MakeError("search_text arguments must be a JSON object");
    }
    for (const auto& item : arguments.items())
    {
        if (!IsSearchTextArgument(item.key()))
        {
            return MakeError("unknown argument: " + item.key());
        }
    }

    if (!arguments.contains("query") || !arguments["query"].is_string())
    {
        return MakeError("search_text requires a string query");
    }
    const std::string query = arguments["query"].get<std::string>();
    if (query.empty())
    {
        return MakeError("query must not be empty");
    }

    std::string relative_path = ".";
    if (arguments.contains("path"))
    {
        if (!arguments["path"].is_string())
        {
            return MakeError("search_text path must be a string");
        }
        relative_path = arguments["path"].get<std::string>();
    }

    std::size_t max_results = m_max_results;
    if (arguments.contains("max_results"))
    {
        const nlohmann::json& value = arguments["max_results"];
        if (!value.is_number_integer())
        {
            return MakeError("max_results must be a positive integer");
        }
        if (IsNegativeJsonInteger(value) || value.get<std::uint64_t>() == 0)
        {
            return MakeError("max_results must be a positive integer");
        }
        max_results = static_cast<std::size_t>(value.get<std::uint64_t>());
    }

    std::filesystem::path start;
    std::string path_error;
    if (!ResolveWorkspacePath(m_policy, relative_path, start, path_error))
    {
        return MakeError(std::move(path_error));
    }

    std::vector<ScanTarget> targets;
    std::string collect_error;
    if (!CollectTargets(start, m_policy, targets, collect_error))
    {
        return MakeError(collect_error + ": " + relative_path);
    }

    std::vector<std::string> matches;
    bool results_truncated = false;
    bool scan_truncated = false;
    std::size_t scanned_files = 0;
    for (const ScanTarget& target : targets)
    {
        if (scanned_files >= m_max_files)
        {
            scan_truncated = true;
            break;
        }
        ++scanned_files;
        if (!ScanFile(target, query, m_max_file_bytes, max_results, matches))
        {
            results_truncated = true;
            break;
        }
    }

    core::ToolResult result;
    result.content = BuildContent(matches, results_truncated, scan_truncated);
    return result;
}

} // namespace agent::plugins::tools
