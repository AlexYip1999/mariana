#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "agent/core/chat_model.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/plugins/metrics/metrics_collector.h"

namespace agent::hosts::cli
{

/// 执行一次单轮问答；返回进程退出码（0 成功，1 调用失败，2 用法错误）。
int RunCli(const std::vector<std::string>& args, agent::core::ChatModel& chat_model, const CliConfig& config,
           std::ostream& out, std::ostream& err, agent::plugins::metrics::MetricsCollector* metrics = nullptr);

} // namespace agent::hosts::cli
