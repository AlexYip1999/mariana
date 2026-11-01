#include "agent/plugins/tools/read_file_tool.h"

#include <exception>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

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

bool IsWithinWorkspace(const std::filesystem::path& canonical_root, const std::filesystem::path& candidate)
{
    const std::filesystem::path relative = candidate.lexically_relative(canonical_root);
    if (relative.empty())
    {
        return false;
    }

    const std::filesystem::path first = *relative.begin();
    return first != "..";
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

} // namespace

ReadFileTool::ReadFileTool(std::filesystem::path workspace_root, std::uintmax_t max_read_bytes)
    : m_workspace_root(std::move(workspace_root))
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

    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(m_workspace_root, error);
    if (error)
    {
        return MakeError("cannot resolve workspace");
    }

    const std::filesystem::path candidate = std::filesystem::weakly_canonical(canonical_root / relative, error);
    if (error)
    {
        return MakeError("cannot resolve path: " + relative_path);
    }
    if (!IsWithinWorkspace(canonical_root, candidate))
    {
        return MakeError("path escapes workspace");
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

    const std::uintmax_t file_size = std::filesystem::file_size(candidate, error);
    if (error)
    {
        return MakeError("cannot read file size: " + relative_path);
    }
    if (file_size > m_max_read_bytes)
    {
        return MakeError("file is too large");
    }

    std::ifstream file(candidate, std::ios::binary);
    if (!file)
    {
        return MakeError("cannot open file: " + relative_path);
    }

    std::ostringstream content;
    content << file.rdbuf();
    if (!file.good() && !file.eof())
    {
        return MakeError("cannot read file: " + relative_path);
    }

    core::ToolResult result;
    result.content = content.str();
    if (result.content.size() > m_max_read_bytes)
    {
        return MakeError("file is too large");
    }
    if (!IsValidUtf8(result.content))
    {
        return MakeError("file is not valid UTF-8 text");
    }
    return result;
}

} // namespace agent::plugins::tools
