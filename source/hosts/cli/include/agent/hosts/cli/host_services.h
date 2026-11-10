#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "agent/core/chat_model.h"
#include "agent/core/memory.h"
#include "agent/core/system_registry.h"
#include "agent/hosts/cli/cli_command.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/runtime/service_registry.h"

namespace agent::plugins::metrics
{
class MetricsCollector;
}

namespace agent::plugins::traces
{
class FileTracer;
}

namespace agent::transport
{
class HttpTransport;
}

namespace agent::hosts::cli
{

class ToolCatalog;

/// Host 组合阶段需要的平台/配置输入。
struct HostServicesConfig
{
    // 配置与运行输入
    CliConfig config;
    std::filesystem::path workspace;
    std::string session_id;
    std::string api_key;

    // 依赖组件
    std::shared_ptr<CommandRegistry> commands;
    std::shared_ptr<ToolCatalog> tool_catalog;
    std::shared_ptr<transport::HttpTransport> transport;
};

/// Host 共享服务与会话作用域；不包含 Agent 和 executor。
struct HostServices
{
    // 模型与两级服务作用域（host / 会话）
    std::shared_ptr<core::ChatModel> model;
    std::shared_ptr<runtime::ServiceRegistry> host_scope;
    std::shared_ptr<runtime::ServiceRegistry> session_scope;

    // 系统注册表与可观测性组件
    std::shared_ptr<core::SystemRegistry> systems;
    std::shared_ptr<plugins::metrics::MetricsCollector> metrics;
    std::shared_ptr<plugins::traces::FileTracer> tracer;

    // 长期记忆（空 = 未启用；通过 --memory / memory_path 打开 JSONL 存储）
    std::shared_ptr<core::MemoryStore> memory;

    // CLI 上下文与工具目录
    std::shared_ptr<CliContext> context;
    std::shared_ptr<ToolCatalog> tool_catalog;
};

/// 组装 model / metrics / tracer / systems / scoped registry / CliContext。
std::optional<HostServices> BuildHostServices(const HostServicesConfig& input, std::string& error);

} // namespace agent::hosts::cli
