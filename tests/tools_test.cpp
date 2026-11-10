#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/core/tool.h"
#include "agent/plugins/tools/read_file_tool.h"

namespace
{

class TempWorkspace
{
  public:
    TempWorkspace()
    {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() / ("agent-core-tools-" + std::to_string(stamp));
        std::filesystem::create_directories(m_path);
    }

    ~TempWorkspace()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

void WriteFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream file(path, std::ios::binary);
    file << content;
}

} // namespace

TEST_CASE("reads a file inside the workspace")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / "hello.txt", "hello");

    agent::plugins::tools::ReadFileTool tool(workspace.Path());
    const agent::core::ToolResult result = tool.Execute(R"({"path":"hello.txt"})");

    CHECK(tool.Name() == "read_file");
    CHECK(tool.Spec().parameters_json.find("\"path\"") != std::string::npos);
    CHECK(result.ok);
    CHECK(result.content == "hello");
}

TEST_CASE("reads Unicode paths independently of the Windows code page")
{
    TempWorkspace workspace;
    const auto directory = workspace.Path() / std::filesystem::path(u8"目录😀");
    std::filesystem::create_directories(directory);
    WriteFile(directory / std::filesystem::path(u8"中😀.txt"), "unicode file");
    WriteFile(directory / ".ENV", "SECRET=1");

    agent::plugins::tools::ReadFileTool tool(workspace.Path());
    const auto result = tool.Execute(R"({"path":"目录😀/中😀.txt"})");

    CHECK(result.ok);
    CHECK(result.content == "unicode file");
    CHECK_FALSE(tool.Execute(R"({"path":"目录😀/.ENV"})").ok);
}

TEST_CASE("rejects symlink escape")
{
    TempWorkspace workspace;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path outside =
        std::filesystem::temp_directory_path() / ("agent-core-tools-outside-" + std::to_string(stamp));
    std::filesystem::create_directories(outside);
    WriteFile(outside / "secret.txt", "secret");

    std::error_code error;
    std::filesystem::create_directory_symlink(outside, workspace.Path() / "link", error);
    if (error)
    {
        std::filesystem::remove_all(outside);
        WARN("symlink creation is not available");
        return;
    }

    agent::plugins::tools::ReadFileTool tool(workspace.Path());
    const agent::core::ToolResult result = tool.Execute(R"({"path":"link/secret.txt"})");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("escapes workspace") != std::string::npos);

    std::filesystem::remove_all(outside);
}

TEST_CASE("rejects sensitive files")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / ".env", "SECRET=1");
    WriteFile(workspace.Path() / "config.local.json", "{}");
    WriteFile(workspace.Path() / "private.key", "-----BEGIN KEY-----");

    agent::plugins::tools::ReadFileTool tool(workspace.Path());

    const std::vector<std::string> sensitive_files = {".env", "config.local.json", "private.key"};
    for (const std::string& name : sensitive_files)
    {
        const std::string arguments = std::string("{\"path\":\"") + name + "\"}";
        const agent::core::ToolResult result = tool.Execute(arguments);

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("sensitive file") != std::string::npos);
    }
}

TEST_CASE("rejects nested and case-variant sensitive files")
{
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.Path() / "sub");
    WriteFile(workspace.Path() / "sub" / ".env", "SECRET=1");
    WriteFile(workspace.Path() / ".ENV", "SECRET=1");
    WriteFile(workspace.Path() / "Private.KEY", "-----BEGIN KEY-----");

    agent::plugins::tools::ReadFileTool tool(workspace.Path());

    const std::vector<std::string> names = {R"({"path":"sub/.env"})", R"({"path":".ENV"})",
                                            R"({"path":"Private.KEY"})"};
    for (const std::string& arguments : names)
    {
        const agent::core::ToolResult result = tool.Execute(arguments);

        CHECK_FALSE(result.ok);
        CHECK(result.error.find("sensitive file") != std::string::npos);
    }
}

TEST_CASE("rejects binary files")
{
    TempWorkspace workspace;
    std::ofstream file(workspace.Path() / "binary.bin", std::ios::binary);
    const char bytes[] = {static_cast<char>(0x80), static_cast<char>(0x81)};
    file.write(bytes, sizeof(bytes));
    file.close();

    agent::plugins::tools::ReadFileTool tool(workspace.Path());
    const agent::core::ToolResult result = tool.Execute(R"({"path":"binary.bin"})");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("UTF-8") != std::string::npos);
}

TEST_CASE("rejects invalid read_file arguments")
{
    TempWorkspace workspace;
    agent::plugins::tools::ReadFileTool tool(workspace.Path());

    const agent::core::ToolResult invalid_json = tool.Execute("{");

    CHECK_FALSE(invalid_json.ok);
    CHECK(invalid_json.error.find("invalid arguments JSON") != std::string::npos);

    const agent::core::ToolResult missing_path = tool.Execute("{}");

    CHECK_FALSE(missing_path.ok);
    CHECK(missing_path.error.find("requires a string path") != std::string::npos);

    const agent::core::ToolResult wrong_type = tool.Execute(R"({"path":123})");

    CHECK_FALSE(wrong_type.ok);
    CHECK(wrong_type.error.find("requires a string path") != std::string::npos);
}

TEST_CASE("rejects absolute paths and workspace escapes")
{
    TempWorkspace workspace;
    const auto outside = std::filesystem::temp_directory_path() / "agent-core-tools-outside.txt";
    WriteFile(outside, "outside");

    agent::plugins::tools::ReadFileTool tool(workspace.Path());

    const nlohmann::json absolute_args{
        {
            "path",
            outside.string(),
        },
    };
    const agent::core::ToolResult absolute = tool.Execute(absolute_args.dump());

    CHECK_FALSE(absolute.ok);
    CHECK(absolute.error.find("absolute paths") != std::string::npos);

    const agent::core::ToolResult escape = tool.Execute(R"({"path":"../agent-core-tools-outside.txt"})");

    CHECK_FALSE(escape.ok);
    CHECK(escape.error.find("escapes workspace") != std::string::npos);

    std::error_code error;
    std::filesystem::remove(outside, error);
}

TEST_CASE("rejects directories and oversized files")
{
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.Path() / "subdir");
    WriteFile(workspace.Path() / "large.txt", "hello world");

    agent::plugins::tools::ReadFileTool tool(workspace.Path(), 4);

    const agent::core::ToolResult directory = tool.Execute(R"({"path":"subdir"})");

    CHECK_FALSE(directory.ok);
    CHECK(directory.error.find("not a regular file") != std::string::npos);

    const agent::core::ToolResult too_large = tool.Execute(R"({"path":"large.txt"})");

    CHECK_FALSE(too_large.ok);
    CHECK(too_large.error.find("too large") != std::string::npos);

    const agent::core::ToolResult missing = tool.Execute(R"({"path":"missing.txt"})");

    CHECK_FALSE(missing.ok);
    CHECK((missing.error.find("file not found") != std::string::npos ||
           missing.error.find("cannot inspect file") != std::string::npos));
}
