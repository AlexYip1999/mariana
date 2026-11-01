#pragma once

#include <string>
#include <vector>

namespace agent::hosts::cli
{

enum class CliMode
{
    Chat,
    Interactive,
    InitConfig,
    Version,
    Help,
};

struct CliOptions
{
    bool init_config = false;
    bool stats = false;
    bool no_tools = false;
    bool trust_workspace_config = false;
    bool trace = false;
    CliMode mode = CliMode::Chat;
    std::string config_path;
    std::string workspace_root;
    std::string trace_dir = "traces";
    std::vector<std::string> question;
};

struct CliParseResult
{
    bool ok = true;
    CliOptions options;
    std::string error;
};

/// 解析命令行参数（不含 argv[0]）。
CliParseResult ParseCliOptions(const std::vector<std::string>& args);

} // namespace agent::hosts::cli
