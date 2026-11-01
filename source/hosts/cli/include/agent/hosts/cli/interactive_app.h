#pragma once

#include <iosfwd>

#include "agent/hosts/cli/cli_command.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/service_registry.h"

namespace agent::hosts::cli
{

/// 交互式终端聊天：从 in 逐行读取用户输入，交给 Agent 处理。
/// 命令从 ServiceRegistry 解析 CommandRegistry + CliContext；未注入时返回错误。
int RunInteractive(runtime::Agent& agent, std::istream& in, std::ostream& out, std::ostream& err,
                   runtime::ServiceRegistry& services, bool auto_print_stats = false);

} // namespace agent::hosts::cli
