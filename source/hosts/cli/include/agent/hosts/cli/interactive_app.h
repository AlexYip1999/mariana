#pragma once

#include <iosfwd>

#include "agent/core/model_adapter.h"
#include "agent/hosts/cli/cli_config.h"

namespace agent::hosts::cli
{

/// 交互式终端聊天：从 in 逐行读取用户输入，维护多轮历史并输出回复。
/// 支持 /help、/clear、/exit、/quit；返回进程退出码（0 正常退出）。
int RunInteractive(agent::core::ModelAdapter& model_adapter, const CliConfig& config, std::istream& in,
                   std::ostream& out, std::ostream& err);

} // namespace agent::hosts::cli
