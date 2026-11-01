#pragma once

#include <iosfwd>

#include "agent/hosts/cli/cli_config.h"
#include "agent/runtime/agent_executor.h"
#include "agent/runtime/service_registry.h"

namespace agent::hosts::cli
{

/// 交互式终端聊天：从 in 逐行读取用户输入，维护多轮历史并输出回复。
/// 支持 /help、/clear、/stats、/exit、/quit；返回进程退出码（0 正常退出）。
int RunInteractive(runtime::AgentExecutor& executor, const CliConfig& config, std::istream& in, std::ostream& out,
                   std::ostream& err, runtime::ServiceRegistry& services, bool auto_print_stats = false);

} // namespace agent::hosts::cli
