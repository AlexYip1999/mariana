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
    CliMode mode = CliMode::Chat;
    std::string config_path;
    std::vector<std::string> question;
    bool stats = false;
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
