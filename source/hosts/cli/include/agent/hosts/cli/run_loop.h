#pragma once

#include <iosfwd>

#include "agent/runtime/agent.h"
#include "agent/runtime/service_registry.h"

namespace agent::hosts::cli
{

/// CLI 运行循环：交互命令 + 用户输入；组合根只负责装配后交给它。
int RunCliLoop(runtime::Agent& agent, runtime::ServiceRegistry& services, std::istream& in, std::ostream& out,
               std::ostream& err, bool auto_print_stats = false);

} // namespace agent::hosts::cli
