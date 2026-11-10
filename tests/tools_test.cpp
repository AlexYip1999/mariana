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
#include "agent/plugins/tools/list_dir_tool.h"
#include "agent/plugins/tools/read_file_tool.h"
#include "agent/plugins/tools/search_text_tool.h"

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

TEST_CASE("lists a directory in sorted order")
{
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.Path() / "subdir");
    WriteFile(workspace.Path() / "b.txt", "12");
    WriteFile(workspace.Path() / "a.txt", "12345");

    agent::plugins::tools::ListDirTool tool(workspace.Path());
    const agent::core::ToolResult result = tool.Execute("{}");

    CHECK(tool.Name() == "list_dir");
    CHECK(tool.Spec().parameters_json.find("\"path\"") != std::string::npos);
    CHECK(result.ok);
    CHECK(result.content == "file a.txt (5 bytes)\nfile b.txt (2 bytes)\ndir subdir");
}

TEST_CASE("lists a subdirectory")
{
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.Path() / "subdir");
    WriteFile(workspace.Path() / "subdir" / "one.txt", "one");
    WriteFile(workspace.Path() / "root.txt", "root");

    agent::plugins::tools::ListDirTool tool(workspace.Path());
    const agent::core::ToolResult result = tool.Execute(R"({"path":"subdir"})");

    CHECK(result.ok);
    CHECK(result.content == "file one.txt (3 bytes)");
}

TEST_CASE("list_dir truncates entries and skips sensitive files")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / "a.txt", "a");
    WriteFile(workspace.Path() / "b.txt", "b");
    WriteFile(workspace.Path() / "c.txt", "c");
    WriteFile(workspace.Path() / ".env", "SECRET=1");

    agent::plugins::tools::ListDirTool tool(workspace.Path(), 2);
    const agent::core::ToolResult result = tool.Execute("{}");

    CHECK(result.ok);
    CHECK(result.content == "file a.txt (1 bytes)\nfile b.txt (1 bytes)\n... (1 more entries)");
    CHECK(result.content.find(".env") == std::string::npos);
}

TEST_CASE("list_dir skips symlinks")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / "real.txt", "real");
    std::error_code error;
    std::filesystem::create_symlink(workspace.Path() / "real.txt", workspace.Path() / "link.txt", error);
    if (error)
    {
        WARN("symlink creation is not available");
        return;
    }

    agent::plugins::tools::ListDirTool tool(workspace.Path());
    const agent::core::ToolResult result = tool.Execute("{}");

    CHECK(result.ok);
    CHECK(result.content == "file real.txt (4 bytes)");
}

TEST_CASE("list_dir rejects invalid paths")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / "file.txt", "text");

    agent::plugins::tools::ListDirTool tool(workspace.Path());

    const nlohmann::json absolute_args{
        {
            "path",
            (workspace.Path() / "subdir").string(),
        },
    };
    const agent::core::ToolResult absolute = tool.Execute(absolute_args.dump());

    CHECK_FALSE(absolute.ok);
    CHECK(absolute.error.find("absolute paths") != std::string::npos);

    const agent::core::ToolResult escape = tool.Execute(R"({"path":"../outside"})");

    CHECK_FALSE(escape.ok);
    CHECK(escape.error.find("escapes workspace") != std::string::npos);

    const agent::core::ToolResult sensitive = tool.Execute(R"({"path":".env"})");

    CHECK_FALSE(sensitive.ok);
    CHECK(sensitive.error.find("sensitive") != std::string::npos);

    const agent::core::ToolResult not_directory = tool.Execute(R"({"path":"file.txt"})");

    CHECK_FALSE(not_directory.ok);
    CHECK(not_directory.error.find("not a directory") != std::string::npos);

    const agent::core::ToolResult missing = tool.Execute(R"({"path":"missing"})");

    CHECK_FALSE(missing.ok);
    CHECK(missing.error.find("not found") != std::string::npos);

    CHECK_FALSE(tool.Execute(R"({"path":123})").ok);
    CHECK_FALSE(tool.Execute(R"({"other":"x"})").ok);
    CHECK_FALSE(tool.Execute("{").ok);
}

TEST_CASE("search_text reports matching line numbers in one file")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / "notes.txt", "alpha\nBeta\ngamma alpha\n");

    agent::plugins::tools::SearchTextTool tool(workspace.Path());
    const agent::core::ToolResult result = tool.Execute(R"({"query":"alpha","path":"notes.txt"})");

    CHECK(tool.Name() == "search_text");
    CHECK(tool.Spec().parameters_json.find("\"query\"") != std::string::npos);
    CHECK(result.ok);
    CHECK(result.content == "notes.txt:1: alpha\nnotes.txt:3: gamma alpha");

    const agent::core::ToolResult case_sensitive = tool.Execute(R"({"query":"beta","path":"notes.txt"})");

    CHECK(case_sensitive.ok);
    CHECK(case_sensitive.content.empty());
}

TEST_CASE("search_text scans files in relative path order")
{
    TempWorkspace workspace;
    std::filesystem::create_directories(workspace.Path() / "sub");
    WriteFile(workspace.Path() / "b.txt", "needle b\n");
    WriteFile(workspace.Path() / "a.txt", "needle a\n");
    WriteFile(workspace.Path() / "sub" / "c.txt", "needle c\n");
    WriteFile(workspace.Path() / "crlf.txt", "one\r\nneedle two\r\n");

    agent::plugins::tools::SearchTextTool tool(workspace.Path());
    const agent::core::ToolResult result = tool.Execute(R"({"query":"needle"})");

    CHECK(result.ok);
    const std::string expected = "a.txt:1: needle a\nb.txt:1: needle b\ncrlf.txt:2: needle two\nsub/c.txt:1: needle c";
    CHECK(result.content == expected);
}

TEST_CASE("search_text truncates results")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / "many.txt", "match\nmatch\nmatch\n");

    agent::plugins::tools::SearchTextTool tool(workspace.Path(), 256 * 1024, 2);
    const agent::core::ToolResult result = tool.Execute(R"({"query":"match"})");

    CHECK(result.ok);
    CHECK(result.content == "many.txt:1: match\nmany.txt:2: match\n... (results truncated)");
}

TEST_CASE("search_text truncates scanned files")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / "a.txt", "hit\n");
    WriteFile(workspace.Path() / "b.txt", "hit\n");

    agent::plugins::tools::SearchTextTool tool(workspace.Path(), 256 * 1024, 50, 1);
    const agent::core::ToolResult result = tool.Execute(R"({"query":"hit"})");

    CHECK(result.ok);
    CHECK(result.content == "a.txt:1: hit\n... (scan truncated)");
}

TEST_CASE("search_text skips sensitive binary and oversized files")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / ".env", "needle\n");
    WriteFile(workspace.Path() / "large.txt", "needle oversized padding");
    WriteFile(workspace.Path() / "small.txt", "needle here\n");
    {
        std::ofstream file(workspace.Path() / "binary.bin", std::ios::binary);
        file << "needle";
        const char nul = '\0';
        file.write(&nul, 1);
    }

    agent::plugins::tools::SearchTextTool tool(workspace.Path(), 16);
    const agent::core::ToolResult result = tool.Execute(R"({"query":"needle"})");

    CHECK(result.ok);
    CHECK(result.content == "small.txt:1: needle here");
}

TEST_CASE("search_text skips symlinks")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / "real.txt", "needle\n");
    std::error_code error;
    std::filesystem::create_symlink(workspace.Path() / "real.txt", workspace.Path() / "link.txt", error);
    if (error)
    {
        WARN("symlink creation is not available");
        return;
    }

    agent::plugins::tools::SearchTextTool tool(workspace.Path());
    const agent::core::ToolResult result = tool.Execute(R"({"query":"needle"})");

    CHECK(result.ok);
    CHECK(result.content == "real.txt:1: needle");
}

TEST_CASE("search_text rejects invalid arguments")
{
    TempWorkspace workspace;
    WriteFile(workspace.Path() / "file.txt", "text");

    agent::plugins::tools::SearchTextTool tool(workspace.Path());

    const agent::core::ToolResult missing_query = tool.Execute(R"({"path":"."})");

    CHECK_FALSE(missing_query.ok);
    CHECK(missing_query.error.find("query") != std::string::npos);

    const agent::core::ToolResult empty_query = tool.Execute(R"({"query":""})");

    CHECK_FALSE(empty_query.ok);
    CHECK(empty_query.error.find("empty") != std::string::npos);

    const agent::core::ToolResult wrong_query_type = tool.Execute(R"({"query":123})");

    CHECK_FALSE(wrong_query_type.ok);

    const agent::core::ToolResult wrong_max_results = tool.Execute(R"({"query":"x","max_results":0})");

    CHECK_FALSE(wrong_max_results.ok);
    CHECK(wrong_max_results.error.find("max_results") != std::string::npos);

    const agent::core::ToolResult unknown_argument = tool.Execute(R"({"query":"x","regex":true})");

    CHECK_FALSE(unknown_argument.ok);
    CHECK(unknown_argument.error.find("unknown argument") != std::string::npos);

    nlohmann::json absolute_args;
    absolute_args["query"] = "x";
    absolute_args["path"] = (workspace.Path() / "file.txt").string();
    const agent::core::ToolResult absolute = tool.Execute(absolute_args.dump());

    CHECK_FALSE(absolute.ok);
    CHECK(absolute.error.find("absolute paths") != std::string::npos);

    const agent::core::ToolResult escape = tool.Execute(R"({"query":"x","path":"../outside"})");

    CHECK_FALSE(escape.ok);
    CHECK(escape.error.find("escapes workspace") != std::string::npos);

    const agent::core::ToolResult sensitive = tool.Execute(R"({"query":"x","path":".env"})");

    CHECK_FALSE(sensitive.ok);
    CHECK(sensitive.error.find("sensitive") != std::string::npos);

    const agent::core::ToolResult missing_path = tool.Execute(R"({"query":"x","path":"missing.txt"})");

    CHECK_FALSE(missing_path.ok);
    CHECK(missing_path.error.find("not found") != std::string::npos);

    CHECK_FALSE(tool.Execute("{").ok);
}
