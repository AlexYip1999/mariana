#include "agent/hosts/cli/cli_config.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent::hosts::cli
{

namespace
{

// 禁止把密钥写进配置文件：只允许环境变量名（api_key_env）或后续的 OS 凭据库。
constexpr const char* FORBIDDEN_CONFIG_FIELDS[] = {"api_key", "key", "token"};

// 共享默认配置（提交到仓库）与本地覆盖配置（gitignore）。
constexpr const char* SHARED_CONFIG_FILE = "config.json";
constexpr const char* LOCAL_CONFIG_FILE = "config.local.json";

std::string FindFirstConfigFile(const std::vector<std::string>& directories, const char* file_name,
                                std::string& error_message)
{
    for (const std::string& directory : directories)
    {
        std::error_code error;
        const std::filesystem::path candidate =
            directory.empty() ? std::filesystem::path(file_name) : std::filesystem::path(directory) / file_name;
        const bool exists = std::filesystem::exists(candidate, error);
        if (error)
        {
            error_message = "cannot inspect config path " + candidate.string() + ": " + error.message();
            return {};
        }
        if (exists)
        {
            return candidate.string();
        }
    }
    return {};
}

std::vector<std::string> BuildConfigDirectories(const std::string& executable_path)
{
    std::vector<std::string> directories;
    directories.emplace_back();
    if (executable_path.empty())
    {
        return directories;
    }

    std::error_code error;
    const std::filesystem::path absolute_executable = std::filesystem::absolute(executable_path, error);
    if (error)
    {
        return directories;
    }

    const std::filesystem::path executable_dir = absolute_executable.parent_path();
    if (!executable_dir.empty())
    {
        directories.push_back(executable_dir.string());
    }
    return directories;
}

} // namespace

ConfigLoadResult ParseCliConfig(const std::string& json_text)
{
    return ParseCliConfig(json_text, CliConfig{});
}

ConfigLoadResult ParseCliConfig(const std::string& json_text, const CliConfig& base)
{
    ConfigLoadResult result;
    result.config = base;
    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(json_text);
        if (!parsed.is_object())
        {
            result.ok = false;
            result.error = "config root must be a JSON object";
            return result;
        }

        for (const char* field : FORBIDDEN_CONFIG_FIELDS)
        {
            if (parsed.contains(field))
            {
                result.ok = false;
                result.error = std::string("do not store API keys in config (field: ") + field +
                               "); use environment variable api_key_env or an OS credential store";
                return result;
            }
        }

        result.config.provider = parsed.value("provider", result.config.provider);
        if (result.config.provider.empty())
        {
            result.ok = false;
            result.error = "provider must not be empty";
            return result;
        }

        result.config.cost_currency = parsed.value("cost_currency", result.config.cost_currency);
        if (result.config.cost_currency != "CNY" && result.config.cost_currency != "USD")
        {
            result.ok = false;
            result.error = "cost_currency must be CNY or USD";
            return result;
        }

        result.config.endpoint = parsed.value("endpoint", result.config.endpoint);
        result.config.model = parsed.value("model", result.config.model);
        result.config.api_key_env = parsed.value("api_key_env", result.config.api_key_env);
        result.config.system_prompt = parsed.value("system_prompt", result.config.system_prompt);
        result.config.timeout_ms = parsed.value("timeout_ms", result.config.timeout_ms);
        result.config.max_tokens = parsed.value("max_tokens", result.config.max_tokens);
        result.config.temperature = parsed.value("temperature", result.config.temperature);

        if (parsed.contains("tool_names"))
        {
            if (!parsed["tool_names"].is_array())
            {
                result.ok = false;
                result.error = "tool_names must be an array of strings";
                return result;
            }

            result.config.tool_names.clear();
            for (const nlohmann::json& item : parsed["tool_names"])
            {
                if (!item.is_string() || item.get<std::string>().empty())
                {
                    result.ok = false;
                    result.error = "tool_names must contain non-empty strings";
                    return result;
                }
                result.config.tool_names.push_back(item.get<std::string>());
            }
        }
    }
    catch (const nlohmann::json::exception& error)
    {
        result.ok = false;
        result.error = error.what();
    }
    return result;
}

ConfigLoadResult LoadCliConfigFile(const std::string& path)
{
    return LoadCliConfigFile(path, CliConfig{});
}

ConfigLoadResult LoadCliConfigFile(const std::string& path, const CliConfig& base)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        ConfigLoadResult result;
        result.ok = false;
        result.error = "cannot open config file: " + path;
        return result;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return ParseCliConfig(buffer.str(), base);
}

DefaultConfigResult LoadDefaultCliConfig(const std::string& executable_path)
{
    DefaultConfigResult result;

    const std::vector<std::string> directories = BuildConfigDirectories(executable_path);
    std::string error_message;

    result.shared_path = FindFirstConfigFile(directories, SHARED_CONFIG_FILE, error_message);
    if (!error_message.empty())
    {
        result.ok = false;
        result.path = result.shared_path;
        result.error = error_message;
        return result;
    }

    result.local_path = FindFirstConfigFile(directories, LOCAL_CONFIG_FILE, error_message);
    if (!error_message.empty())
    {
        result.ok = false;
        result.path = result.local_path;
        result.error = error_message;
        return result;
    }

    CliConfig config;
    if (!result.shared_path.empty())
    {
        const ConfigLoadResult loaded = LoadCliConfigFile(result.shared_path, config);
        if (!loaded.ok)
        {
            result.ok = false;
            result.path = result.shared_path;
            result.error = loaded.error;
            return result;
        }
        config = loaded.config;
    }

    if (!result.local_path.empty())
    {
        const ConfigLoadResult loaded = LoadCliConfigFile(result.local_path, config);
        if (!loaded.ok)
        {
            result.ok = false;
            result.path = result.local_path;
            result.error = loaded.error;
            return result;
        }
        config = loaded.config;
    }

    result.found = !result.shared_path.empty() || !result.local_path.empty();
    result.path = result.local_path.empty() ? result.shared_path : result.local_path;
    result.config = config;
    return result;
}

ConfigInitResult WriteDefaultCliConfigFile(const std::string& path)
{
    ConfigInitResult result;
    result.path = path;

    std::error_code error;
    if (std::filesystem::exists(path, error))
    {
        result.ok = false;
        result.error = "config file already exists: " + path;
        return result;
    }
    if (error)
    {
        result.ok = false;
        result.error = "cannot inspect config path: " + error.message();
        return result;
    }

    const CliConfig defaults;
    nlohmann::json config_json;
    config_json["provider"] = defaults.provider;
    config_json["cost_currency"] = defaults.cost_currency;
    config_json["endpoint"] = defaults.endpoint;
    config_json["model"] = defaults.model;
    config_json["api_key_env"] = defaults.api_key_env;
    config_json["system_prompt"] = defaults.system_prompt;
    config_json["timeout_ms"] = defaults.timeout_ms;
    config_json["max_tokens"] = defaults.max_tokens;
    config_json["temperature"] = defaults.temperature;
    config_json["tool_names"] = defaults.tool_names;

    std::ofstream file(path);
    if (!file.is_open())
    {
        result.ok = false;
        result.error = "cannot write config file: " + path;
        return result;
    }

    file << config_json.dump(2) << "\n";
    if (!file)
    {
        result.ok = false;
        result.error = "failed to write config file: " + path;
    }
    return result;
}

std::string ResolveApiKey(const CliConfig& config, const std::function<std::string(const std::string&)>& get_env)
{
    if (config.api_key_env.empty() || !get_env)
    {
        return {};
    }
    return get_env(config.api_key_env);
}

} // namespace agent::hosts::cli
