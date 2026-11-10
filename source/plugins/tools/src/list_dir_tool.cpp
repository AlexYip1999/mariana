#include "agent/plugins/tools/list_dir_tool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
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

struct DirectoryEntry
{
    std::string name;
    bool is_directory = false;
    std::uintmax_t size = 0;
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

bool ReadDirectoryEntries(const std::filesystem::path& directory, const WorkspacePathPolicy& policy,
                          std::vector<DirectoryEntry>& entries, std::string& error)
{
    entries.clear();
    try
    {
        const std::filesystem::directory_options options = std::filesystem::directory_options::skip_permission_denied;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, options))
        {
            std::error_code entry_error;
            const std::filesystem::file_status status = entry.symlink_status(entry_error);
            if (entry_error || status.type() == std::filesystem::file_type::not_found)
            {
                continue;
            }
            if (std::filesystem::is_symlink(status))
            {
                continue;
            }

            const std::filesystem::path name = entry.path().filename();
            if (policy.IsSensitive(name))
            {
                continue;
            }

            DirectoryEntry item;
            if (std::filesystem::is_directory(status))
            {
                item.is_directory = true;
            }
            else if (std::filesystem::is_regular_file(status))
            {
                const std::uintmax_t size = entry.file_size(entry_error);
                if (entry_error)
                {
                    continue;
                }
                item.size = size;
            }
            else
            {
                // 跳过设备、管道、socket 等特殊文件。
                continue;
            }
            item.name = Utf8String(name);
            entries.push_back(std::move(item));
        }
    }
    catch (const std::exception&)
    {
        error = "cannot list directory";
        return false;
    }

    std::sort(entries.begin(), entries.end(),
              [](const DirectoryEntry& left, const DirectoryEntry& right)
              {
                  return left.name < right.name;
              });
    return true;
}

std::string FormatEntries(const std::vector<DirectoryEntry>& entries, std::size_t max_entries)
{
    std::string content;
    const std::size_t visible = std::min(entries.size(), max_entries);
    for (std::size_t index = 0; index < visible; ++index)
    {
        if (index != 0)
        {
            content.push_back('\n');
        }
        const DirectoryEntry& entry = entries[index];
        if (entry.is_directory)
        {
            content += "dir " + entry.name;
        }
        else
        {
            content += "file " + entry.name + " (" + std::to_string(entry.size) + " bytes)";
        }
    }
    if (entries.size() > visible)
    {
        if (!content.empty())
        {
            content.push_back('\n');
        }
        content += "... (" + std::to_string(entries.size() - visible) + " more entries)";
    }
    return content;
}

} // namespace

ListDirTool::ListDirTool(std::filesystem::path workspace_root, std::size_t max_entries)
    : m_policy(std::move(workspace_root))
    , m_max_entries(max_entries)
{}

std::string ListDirTool::Name() const
{
    return "list_dir";
}

std::string ListDirTool::Description() const
{
    return "List a directory inside the workspace.";
}

std::string ListDirTool::ParametersJson() const
{
    return R"({"type":"object","properties":{"path":{"type":"string","description":"Workspace-relative directory path; defaults to the workspace root."}},"additionalProperties":false})";
}

core::ToolResult ListDirTool::Execute(const std::string& arguments_json, const core::CallContext& context)
{
    if (context.Interruption() != core::CallError::None)
    {
        auto result = MakeError("list_dir interrupted");
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
        return MakeError("list_dir arguments must be a JSON object");
    }
    for (const auto& item : arguments.items())
    {
        if (item.key() != "path")
        {
            return MakeError("unknown argument: " + item.key());
        }
    }
    if (arguments.contains("path") && !arguments["path"].is_string())
    {
        return MakeError("list_dir path must be a string");
    }

    std::string relative_path = ".";
    if (arguments.contains("path"))
    {
        relative_path = arguments["path"].get<std::string>();
    }

    std::filesystem::path directory;
    std::string path_error;
    if (!ResolveWorkspacePath(m_policy, relative_path, directory, path_error))
    {
        return MakeError(std::move(path_error));
    }

    std::error_code status_error;
    const std::filesystem::file_status status = std::filesystem::status(directory, status_error);
    if (status_error)
    {
        // 不存在的路径也可能以 error_code 报告（如 Windows 的 no_such_file_or_directory）。
        if (status_error == std::errc::no_such_file_or_directory)
        {
            return MakeError("directory not found: " + relative_path);
        }
        return MakeError("cannot inspect directory: " + relative_path);
    }
    if (status.type() == std::filesystem::file_type::not_found)
    {
        return MakeError("directory not found: " + relative_path);
    }
    if (!std::filesystem::is_directory(status))
    {
        return MakeError("path is not a directory: " + relative_path);
    }

    std::vector<DirectoryEntry> entries;
    std::string list_error;
    if (!ReadDirectoryEntries(directory, m_policy, entries, list_error))
    {
        return MakeError(list_error + ": " + relative_path);
    }

    core::ToolResult result;
    result.content = FormatEntries(entries, m_max_entries);
    return result;
}

} // namespace agent::plugins::tools
