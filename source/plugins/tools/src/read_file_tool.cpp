#include "agent/plugins/tools/read_file_tool.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

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

bool IsContinuationByte(unsigned char value)
{
    return (value & 0xC0U) == 0x80U;
}

bool IsValidUtf8(const std::string& text)
{
    std::size_t index = 0;
    while (index < text.size())
    {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first == 0)
        {
            return false;
        }
        if (first < 0x80U)
        {
            ++index;
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU)
        {
            if (index + 1 >= text.size() || !IsContinuationByte(static_cast<unsigned char>(text[index + 1])))
            {
                return false;
            }
            index += 2;
            continue;
        }
        if (first >= 0xE0U && first <= 0xEFU)
        {
            if (index + 2 >= text.size() || !IsContinuationByte(static_cast<unsigned char>(text[index + 1])) ||
                !IsContinuationByte(static_cast<unsigned char>(text[index + 2])))
            {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            if ((first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second >= 0xA0U))
            {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xF0U && first <= 0xF4U)
        {
            if (index + 3 >= text.size() || !IsContinuationByte(static_cast<unsigned char>(text[index + 1])) ||
                !IsContinuationByte(static_cast<unsigned char>(text[index + 2])) ||
                !IsContinuationByte(static_cast<unsigned char>(text[index + 3])))
            {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            if ((first == 0xF0U && second < 0x90U) || (first == 0xF4U && second >= 0x90U))
            {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
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

core::ToolResult MakeContentResult(std::string content)
{
    if (!IsValidUtf8(content))
    {
        return MakeError("file is not valid UTF-8 text");
    }

    core::ToolResult result;
    result.content = std::move(content);
    return result;
}

#if defined(_WIN32)

struct HandleDeleter
{
    void operator()(void* handle) const
    {
        if (handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

std::wstring StripLongPathPrefix(std::wstring value)
{
    if (value.rfind(L"\\\\?\\UNC\\", 0) == 0)
    {
        return L"\\\\" + value.substr(8);
    }
    if (value.rfind(L"\\\\?\\", 0) == 0)
    {
        return value.substr(4);
    }
    return value;
}

core::ToolResult ReadWindowsHandle(HANDLE raw_handle, const WorkspacePathPolicy& policy, std::uintmax_t max_read_bytes,
                                   const std::string& relative_path)
{
    UniqueHandle handle(raw_handle);

    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(raw_handle, nullptr, 0, flags);
    if (required == 0)
    {
        return MakeError("cannot resolve opened file: " + relative_path);
    }

    std::wstring buffer(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(raw_handle, buffer.data(), required, flags);
    if (written == 0 || written >= required)
    {
        return MakeError("cannot resolve opened file: " + relative_path);
    }
    buffer.resize(written);

    std::error_code canonical_error;
    const std::filesystem::path final_path = std::filesystem::weakly_canonical(
        std::filesystem::path(StripLongPathPrefix(std::move(buffer))), canonical_error);
    if (canonical_error || !policy.IsWithinWorkspace(final_path))
    {
        return MakeError("path escapes workspace");
    }

    const std::filesystem::path final_relative = final_path.lexically_relative(policy.Root());
    if (policy.IsSensitive(final_relative))
    {
        return MakeError("sensitive file is not readable");
    }

    BY_HANDLE_FILE_INFORMATION file_info{};
    if (!GetFileInformationByHandle(raw_handle, &file_info))
    {
        return MakeError("cannot inspect file: " + relative_path);
    }
    if ((file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return MakeError("path is not a regular file");
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(raw_handle, &file_size) || file_size.QuadPart < 0)
    {
        return MakeError("cannot read file size: " + relative_path);
    }
    if (static_cast<std::uintmax_t>(file_size.QuadPart) > max_read_bytes)
    {
        return MakeError("file is too large");
    }

    std::string content(static_cast<std::size_t>(max_read_bytes) + 1, '\0');
    std::size_t total = 0;
    while (total < content.size())
    {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - total, 64 * 1024));
        DWORD read = 0;
        if (!ReadFile(raw_handle, content.data() + total, chunk, &read, nullptr))
        {
            return MakeError("cannot read file: " + relative_path);
        }
        if (read == 0)
        {
            break;
        }
        total += read;
    }

    if (total > max_read_bytes)
    {
        return MakeError("file is too large");
    }
    content.resize(total);
    return MakeContentResult(std::move(content));
}

core::ToolResult ReadFileByHandle(const std::filesystem::path& candidate, const WorkspacePathPolicy& policy,
                                  std::uintmax_t max_read_bytes, const std::string& relative_path)
{
    HANDLE handle = CreateFileW(candidate.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            return MakeError("file not found: " + relative_path);
        }
        return MakeError("cannot open file: " + relative_path);
    }
    return ReadWindowsHandle(handle, policy, max_read_bytes, relative_path);
}

#else

core::ToolResult ReadFileByStream(const std::filesystem::path& candidate, std::uintmax_t max_read_bytes,
                                  const std::string& relative_path)
{
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(candidate, error);
    if (error)
    {
        return MakeError("cannot inspect file: " + relative_path);
    }
    if (!std::filesystem::is_regular_file(status))
    {
        return MakeError("path is not a regular file");
    }

    const std::uintmax_t file_size = std::filesystem::file_size(candidate, error);
    if (error)
    {
        return MakeError("cannot read file size: " + relative_path);
    }
    if (file_size > max_read_bytes)
    {
        return MakeError("file is too large");
    }

    std::ifstream file(candidate, std::ios::binary);
    if (!file)
    {
        return MakeError("cannot open file: " + relative_path);
    }

    std::string content(static_cast<std::size_t>(max_read_bytes) + 1, '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    const std::streamsize read_count = file.gcount();
    if (!file.eof() && !file.good())
    {
        return MakeError("cannot read file: " + relative_path);
    }
    if (read_count < 0 || static_cast<std::uintmax_t>(read_count) > max_read_bytes)
    {
        return MakeError("file is too large");
    }
    content.resize(static_cast<std::size_t>(read_count));
    return MakeContentResult(std::move(content));
}

#endif

} // namespace

ReadFileTool::ReadFileTool(std::filesystem::path workspace_root, std::uintmax_t max_read_bytes)
    : m_policy(std::move(workspace_root))
    , m_max_read_bytes(max_read_bytes)
{}

std::string ReadFileTool::Name() const
{
    return "read_file";
}

std::string ReadFileTool::Description() const
{
    return "Read a text file inside the workspace.";
}

std::string ReadFileTool::ParametersJson() const
{
    return R"({"type":"object","properties":{"path":{"type":"string","description":"Workspace-relative file path"}},"required":["path"],"additionalProperties":false})";
}

core::ToolResult ReadFileTool::Execute(const std::string& arguments_json)
{
    nlohmann::json arguments;
    try
    {
        arguments = nlohmann::json::parse(arguments_json);
    }
    catch (const nlohmann::json::exception& error)
    {
        return MakeError(std::string("invalid arguments JSON: ") + error.what());
    }

    if (!arguments.is_object() || !arguments.contains("path") || !arguments["path"].is_string())
    {
        return MakeError("read_file requires a string path");
    }

    return ExecuteRead(arguments["path"].get<std::string>());
}

core::ToolResult ReadFileTool::ExecuteRead(const std::string& relative_path) const
{
    if (relative_path.empty())
    {
        return MakeError("path must not be empty");
    }

    std::filesystem::path relative;
    try
    {
        relative = Utf8Path(relative_path);
    }
    catch (const std::exception&)
    {
        return MakeError("invalid UTF-8 path");
    }
    if (relative.is_absolute() || relative.has_root_name())
    {
        return MakeError("absolute paths are not allowed");
    }

    if (!m_policy.IsValid())
    {
        return MakeError("cannot resolve workspace");
    }

    std::error_code error;
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(m_policy.Root() / relative, error);
    if (error)
    {
        return MakeError("cannot resolve path: " + relative_path);
    }
    if (!m_policy.IsWithinWorkspace(candidate))
    {
        return MakeError("path escapes workspace");
    }
    if (m_policy.IsSensitive(candidate.lexically_relative(m_policy.Root())))
    {
        return MakeError("sensitive file is not readable");
    }

    const std::filesystem::file_status status = std::filesystem::status(candidate, error);
    if (error)
    {
        return MakeError("cannot inspect file: " + relative_path);
    }
    if (!std::filesystem::is_regular_file(status))
    {
        return MakeError("path is not a regular file");
    }

#if defined(_WIN32)
    return ReadFileByHandle(candidate, m_policy, m_max_read_bytes, relative_path);
#else
    return ReadFileByStream(candidate, m_max_read_bytes, relative_path);
#endif
}

} // namespace agent::plugins::tools
