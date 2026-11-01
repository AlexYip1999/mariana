#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace agent::hosts::cli
{

/// 最小启动参数：其余选项都通过交互命令设置。
struct CliOptions
{
    bool help = false;
    bool version = false;
    std::string config_path;
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
