#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "agent/core/memory.h"

namespace agent::hosts::cli
{

using EnvironmentLookup = std::function<std::string(const std::string&)>;

/// CLI 长期记忆的固定作用域（跨 session 共享）；由 --memory 开关启用。
inline core::MemoryScope CliMemoryScope()
{
    return core::MemoryScope{"cli", "default"};
}

struct CliConfig
{
    // 服务连接
    std::string provider = "deepseek";
    std::string cost_currency = "CNY";
    std::string endpoint = "https://api.deepseek.com/chat/completions";
    std::string model = "deepseek-flash";
    std::string api_key_env = "DEEPSEEK_API_KEY";

    // 模型与提示词
    std::string system_prompt = "You are a concise assistant.";
    int timeout_ms = 30000;
    int max_tokens = 1024;

    // 运行预算
    int max_steps = 8;
    int turn_timeout_ms = 120000;
    std::size_t max_tool_calls = 64;
    std::size_t max_tool_output_bytes = 32768;

    // 采样与上下文
    double temperature = 0.0;
    std::size_t max_context_chars = 120000;

    // 工具
    std::vector<std::string> tool_names = {"read_file", "list_dir", "search_text"};

    // 长期记忆：JSONL 文件路径；空 = 关闭（可用 --memory 覆盖）
    std::string memory_path;
};

struct ConfigLoadResult
{
    bool ok = true;
    CliConfig config;
    std::string error;
};

struct DefaultConfigResult
{
    // 加载结果
    bool ok = true;
    bool found = false;

    // 配置路径
    // 最终生效的配置路径：local 存在时是 local，否则是 shared；都没有时为空。
    std::string path;
    std::string shared_path;
    std::string local_path;

    // 配置与错误
    CliConfig config;
    std::string error;
};

struct ConfigInitResult
{
    bool ok = true;
    std::string path;
    std::string error;
};

/// 解析 JSON 配置文本（缺失字段使用内置默认值）。
ConfigLoadResult ParseCliConfig(const std::string& json_text);

/// 解析 JSON 配置文本；base 提供缺失字段的默认值，用于共享配置 + 本地覆盖。
ConfigLoadResult ParseCliConfig(const std::string& json_text, const CliConfig& base);

/// 读取配置文件；打开失败或 JSON 非法时 ok=false。
ConfigLoadResult LoadCliConfigFile(const std::string& path);

/// 读取配置文件；base 提供缺失字段的默认值。
ConfigLoadResult LoadCliConfigFile(const std::string& path, const CliConfig& base);

/// 默认配置加载，顺序（后者字段级覆盖前者）：
/// 1. 内置默认值；
/// 2. 共享配置 config.json：先当前工作目录，再 exe 同目录；
/// 3. 本地覆盖 config.local.json：先当前工作目录，再 exe 同目录。
/// executable_path 传 argv[0]，用于定位 exe 同目录；为空时只查当前工作目录。
DefaultConfigResult LoadDefaultCliConfig(const std::string& executable_path = {});

/// 写入默认配置文件（不覆盖已存在文件）；成功时返回写入路径。
ConfigInitResult WriteDefaultCliConfigFile(const std::string& path);

/// 从环境变量取 API key；缺失时返回空字符串。
std::string ResolveApiKey(const CliConfig& config, const EnvironmentLookup& get_env);

} // namespace agent::hosts::cli
