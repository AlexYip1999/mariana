#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "agent/core/model_adapter.h"
#include "agent/hosts/cli/cli_config.h"

namespace agent::hosts::cli
{

/// 执行一次单轮问答；返回进程退出码（0 成功，1 调用失败，2 用法错误）。
int RunCli(const std::vector<std::string>& args, agent::core::ModelAdapter& model_adapter, const CliConfig& config,
           std::ostream& out, std::ostream& err);

} // namespace agent::hosts::cli
